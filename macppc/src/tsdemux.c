#include "tsdemux.h"

#include <stdlib.h>
#include <string.h>

#define PACKET_SIZE 188

typedef struct {
    unsigned             pid;
    int                  payload_start;
    const unsigned char *payload;   /* TS header and adaptation field stripped */
    size_t               payload_len;
} PacketInfo;

typedef struct {
    PacketInfo *items;
    size_t      count;
} PacketList;

/* Walks the whole segment once, byte-resyncing on 0x47 the way the Haiku
 * original did, and records where each packet's payload starts. */
static int parse_packets(const unsigned char *data, size_t len, PacketList *out)
{
    size_t position = 0, capacity = 256;

    out->items = (PacketInfo *)malloc(capacity * sizeof(*out->items));
    out->count = 0;
    if (!out->items)
        return -1;

    while (position + PACKET_SIZE <= len) {
        const unsigned char *packet = data + position;
        unsigned adaptation_field_control, payload_offset;
        PacketInfo *slot;

        if (packet[0] != 0x47) {
            position += 1; /* not aligned - resync byte by byte */
            continue;
        }

        adaptation_field_control = (packet[3] >> 4) & 0x3;
        if (adaptation_field_control == 2) {
            position += PACKET_SIZE; /* adaptation field only, no payload */
            continue;
        }
        payload_offset = (adaptation_field_control == 3)
                             ? (unsigned)(5 + packet[4])
                             : 4u;
        if (payload_offset > PACKET_SIZE) {
            position += PACKET_SIZE; /* malformed */
            continue;
        }

        if (out->count == capacity) {
            PacketInfo *grown =
                (PacketInfo *)realloc(out->items, capacity * 2 * sizeof(*out->items));
            if (!grown) {
                free(out->items);
                out->items = NULL;
                return -1;
            }
            out->items = grown;
            capacity *= 2;
        }
        slot = &out->items[out->count++];
        slot->pid = ((unsigned)(packet[1] & 0x1F) << 8) | packet[2];
        slot->payload_start = (packet[1] & 0x40) != 0;
        slot->payload = packet + payload_offset;
        slot->payload_len = PACKET_SIZE - payload_offset;
        position += PACKET_SIZE;
    }
    return 0;
}

/* Start of a PSI section inside a payload-start packet, past the pointer_field.
 * Returns 0 if the section is shorter than `minimum_length`. */
static int section_of(const PacketInfo *packet, size_t minimum_length,
                      const unsigned char **out, size_t *out_len)
{
    size_t section_start;

    if (packet->payload_len < 1)
        return 0;
    section_start = 1 + (size_t)packet->payload[0];
    if (section_start > packet->payload_len)
        return 0;
    if (packet->payload_len - section_start < minimum_length)
        return 0;
    *out = packet->payload + section_start;
    *out_len = packet->payload_len - section_start;
    return 1;
}

static int find_pmt_pid(const PacketList *packets, unsigned *out_pid)
{
    size_t index;

    for (index = 0; index < packets->count; index++) {
        const PacketInfo *packet = &packets->items[index];
        const unsigned char *section;
        size_t section_len, section_length, program_end, cursor;

        if (packet->pid != 0 || !packet->payload_start)
            continue;
        if (!section_of(packet, 8, &section, &section_len))
            continue;
        if (section[0] != 0x00)
            continue; /* PAT table_id */

        section_length = ((size_t)(section[1] & 0x0F) << 8) | section[2];
        /* 3 header bytes precede what section_length covers; the trailing 4
         * bytes are the CRC32. */
        program_end = 3 + section_length;
        program_end = program_end > 4 ? program_end - 4 : 0;
        if (program_end > section_len)
            program_end = section_len;

        for (cursor = 8; cursor + 4 <= program_end; cursor += 4) {
            unsigned program_number =
                ((unsigned)section[cursor] << 8) | section[cursor + 1];
            if (program_number == 0)
                continue; /* program_number 0 entries are the NIT */
            *out_pid = ((unsigned)(section[cursor + 2] & 0x1F) << 8) | section[cursor + 3];
            return 1;
        }
    }
    return 0;
}

static int find_audio_stream(const PacketList *packets, unsigned pmt_pid,
                             unsigned *out_pid, RwrAudioCodec *out_codec)
{
    size_t index;

    for (index = 0; index < packets->count; index++) {
        const PacketInfo *packet = &packets->items[index];
        const unsigned char *section;
        size_t section_len, section_length, program_info_length, end, cursor;

        if (packet->pid != pmt_pid || !packet->payload_start)
            continue;
        if (!section_of(packet, 12, &section, &section_len))
            continue;
        if (section[0] != 0x02)
            continue; /* PMT table_id */

        section_length = ((size_t)(section[1] & 0x0F) << 8) | section[2];
        program_info_length = ((size_t)(section[10] & 0x0F) << 8) | section[11];
        end = 3 + section_length;
        end = end > 4 ? end - 4 : 0;
        if (end > section_len)
            end = section_len;

        cursor = 12 + program_info_length;
        while (cursor + 5 <= end) {
            unsigned char stream_type = section[cursor];
            size_t es_info_length =
                ((size_t)(section[cursor + 3] & 0x0F) << 8) | section[cursor + 4];
            RwrAudioCodec codec = RWR_CODEC_UNKNOWN;

            if (stream_type == 0x0F)
                codec = RWR_CODEC_ADTS_AAC;
            else if (stream_type == 0x03 || stream_type == 0x04)
                codec = RWR_CODEC_MPEG_AUDIO;

            if (codec != RWR_CODEC_UNKNOWN) {
                *out_pid = ((unsigned)(section[cursor + 1] & 0x1F) << 8) | section[cursor + 2];
                *out_codec = codec;
                return 1;
            }
            cursor += 5 + es_info_length;
        }
    }
    return 0;
}

RwrAudioCodec tsdemux_extract(const unsigned char *data, size_t len, Buf *out)
{
    PacketList packets;
    unsigned pmt_pid = 0, audio_pid = 0;
    RwrAudioCodec codec = RWR_CODEC_UNKNOWN;
    size_t index;
    size_t written_at_entry = out->len;

    if (parse_packets(data, len, &packets) != 0)
        return RWR_CODEC_UNKNOWN;

    if (!find_pmt_pid(&packets, &pmt_pid) ||
        !find_audio_stream(&packets, pmt_pid, &audio_pid, &codec)) {
        free(packets.items);
        return RWR_CODEC_UNKNOWN;
    }

    for (index = 0; index < packets.count; index++) {
        const PacketInfo *packet = &packets.items[index];

        if (packet->pid != audio_pid)
            continue;

        if (packet->payload_start) {
            const unsigned char *payload = packet->payload;
            size_t es_start;

            if (packet->payload_len < 9)
                continue;
            if (!(payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01))
                continue; /* not a PES start code - drop this packet's payload */
            es_start = 9 + (size_t)payload[8]; /* + PES_header_data_length */
            if (es_start < packet->payload_len)
                buf_append(out, payload + es_start, packet->payload_len - es_start);
        } else {
            buf_append(out, packet->payload, packet->payload_len);
        }
    }

    free(packets.items);
    /* A PMT that named an audio PID no packet actually carried is the same
     * "no usable audio" case as an unparseable PMT. */
    if (out->len == written_at_entry)
        return RWR_CODEC_UNKNOWN;
    return codec;
}
