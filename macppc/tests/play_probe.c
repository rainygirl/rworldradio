/* Plays one station from the dataset through the real playback path, with no
 * window.
 *
 * This is the counterpart to tests/decode_probe.c: that one reads a local file
 * and stops at the decoder, this one goes through player.c end to end - the
 * dataset, the URL, TLS, the HLS or progressive pipeline, the decoder and the
 * CoreAudio output unit. Everything the app does except Carbon.
 *
 * It exists because the GUI cannot be driven over SSH - the window server
 * belongs to the console session - while CoreAudio can, so this is the way to
 * check the parts that actually talk to the network and the sound hardware on
 * the target machine.
 *
 *   play_probe <stations.dat> <cacert.der> <country> [seconds]
 *   play_probe <stations.dat> <cacert.der> --list
 *
 * `country` is matched case-insensitively as a substring; the first station of
 * the first match is played. Exit status is 0 only if audio actually started.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dataset.h"
#include "net.h"
#include "player.h"

static int contains_ci(const char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);
    size_t index;

    if (needle_len == 0)
        return 1;
    for (; *haystack; haystack++) {
        for (index = 0; index < needle_len; index++) {
            if (tolower((unsigned char)haystack[index]) !=
                tolower((unsigned char)needle[index]))
                break;
        }
        if (index == needle_len)
            return 1;
    }
    return 0;
}

static const char *event_name(PlayerEventKind kind)
{
    switch (kind) {
    case RWR_EVENT_CONNECTING: return "connecting";
    case RWR_EVENT_PLAYING:    return "playing";
    case RWR_EVENT_STOPPED:    return "stopped";
    case RWR_EVENT_ERROR:      return "error";
    }
    return "?";
}

int main(int argc, char **argv)
{
    Dataset *dataset;
    Player *player;
    Station station;
    char error[512];
    char label[128];
    int countries, index, found = -1, seconds, tick;
    int playing = 0, failed = 0;

    if (argc < 4) {
        fprintf(stderr, "usage: play_probe <stations.dat> <cacert.der> "
                        "<country|--list> [seconds]\n");
        return 2;
    }
    seconds = (argc > 4) ? atoi(argv[4]) : 12;

    dataset = dataset_open(argv[1], error, sizeof(error));
    if (!dataset) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    countries = dataset_country_count(dataset);
    printf("dataset    %d countries, %d stations\n",
           countries, dataset_total_station_count(dataset));

    if (strcmp(argv[3], "--list") == 0) {
        for (index = 0; index < countries; index++)
            printf("  %-40s %d\n", dataset_country_name(dataset, index),
                   dataset_station_count(dataset, index));
        dataset_close(dataset);
        return 0;
    }

    for (index = 0; index < countries; index++) {
        if (contains_ci(dataset_country_name(dataset, index), argv[3])) {
            found = index;
            break;
        }
    }
    if (found < 0 || dataset_station(dataset, found, 0, &station) != 0) {
        fprintf(stderr, "no country matching \"%s\"\n", argv[3]);
        dataset_close(dataset);
        return 1;
    }

    station_codec_bitrate_label(&station, label, sizeof(label));
    printf("country    %s\n", station.country);
    printf("station    %s\n", station.name);
    printf("codec      %s\n", label[0] ? label : "(unknown)");
    printf("url        %s\n", station.url);
    fflush(stdout);

    net_init(argv[2]);
    player = player_new(NULL, NULL);
    if (!player) {
        dataset_close(dataset);
        return 1;
    }

    player_play(player, &station);

    /* Poll the way the UI's 100ms timer does, and print the peak level so the
     * output shows whether real audio is moving rather than just that nothing
     * crashed. */
    for (tick = 0; tick < seconds * 10; tick++) {
        PlayerEvent event;

        while (player_poll_event(player, &event)) {
            printf("[%5.1fs] %-10s %s%s%s\n", tick / 10.0, event_name(event.kind),
                   event.station,
                   event.detail[0] ? " - " : "", event.detail);
            fflush(stdout);
            if (event.kind == RWR_EVENT_PLAYING)
                playing = 1;
            if (event.kind == RWR_EVENT_ERROR)
                failed = 1;
        }
        if (playing && tick % 20 == 0) {
            float level = player_level(player);
            int bars = (int)(level * 40.0f + 0.5f);
            printf("[%5.1fs] level %5.3f |%.*s%*s|\n", tick / 10.0, (double)level,
                   bars, "########################################", 40 - bars, "");
            fflush(stdout);
        }
        if (failed)
            break;
        usleep(100000);
    }

    player_stop(player);
    player_free(player);

    /* Before net_shutdown, which empties the pool. On an HLS station most
     * requests should be `reused`: playlist polls and segment fetches all go to
     * the same host, and each one avoided is a TLS handshake not performed. */
    {
        NetStats net;
        net_stats(&net);
        printf("connections opened %u, reused %u, stale %u\n",
               net.opened, net.reused, net.stale);
        printf("tls handshakes     %u in %.2fs (avg %.0f ms, min %.0f, max %.0f)\n",
               net.handshakes, net.handshake_seconds,
               net.handshakes ? net.handshake_seconds * 1000.0 / net.handshakes : 0.0,
               net.handshake_min * 1000.0, net.handshake_max * 1000.0);
    }

    net_shutdown();
    dataset_close(dataset);

    printf("result     %s\n", failed ? "FAILED" : (playing ? "audio played" : "never started"));
    return (playing && !failed) ? 0 : 1;
}
