/*
 * td_proto_cmds.c - `mqtt` and `modbus` commands for TinyDesk Shell. They drive the
 * same connections as the MQTT and Modbus apps (td_mqtt / td_modbus), so a
 * broker connected from the shell shows up in the app and the other way
 * round.
 *
 * Output goes to the command's stdout (the Terminal window, a Telnet or an
 * SSH session).
 */
#include "td_proto_cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "td_modbus.h"
#include "td_mqtt.h"
#include "td_sock.h"
#include "tdsh.h"

/* ------------------------------------------------------------ helpers */

static bool (*s_break_check)(void);

void td_proto_set_break_check(bool (*fn)(void)) { s_break_check = fn; }

static bool break_requested(void) { return s_break_check && s_break_check(); }

static bool parse_u16(const char *s, uint16_t *out)
{
    char *end = NULL;
    const char *d = s[0] == '-' ? s + 1 : s;
    int base = d[0] == '0' && (d[1] == 'x' || d[1] == 'X') ? 16 : 10;   /* 010 is ten, not octal */
    long v = strtol(s, &end, base);
    if (!s[0] || !end || *end || v < -32768 || v > 65535) return false;
    *out = (uint16_t)v;
    return true;
}

/* No default: with every state listed, -Wswitch reports a new one. */
static const char *state_name(td_mqtt_state_t s)
{
    switch (s) {
    case TD_MQTT_OFF: return "not connected";
    case TD_MQTT_CONNECTING: return "connecting";
    case TD_MQTT_TLS_HANDSHAKE: return "TLS handshake";
    case TD_MQTT_WAIT_CONNACK: return "logging in";
    case TD_MQTT_CONNECTED: return "connected";
    case TD_MQTT_RETRY: return "reconnecting";
    }
    return "not connected";
}

/* Wait (driving the connection) until it settles or timeout_ms passes. */
static td_mqtt_state_t mqtt_wait(uint32_t timeout_ms)
{
    uint32_t start = td_proto_millis();
    td_mqtt_status_t st;
    for (;;) {
        td_mqtt_poll();
        td_mqtt_status(&st);
        if (st.state == TD_MQTT_CONNECTED || st.state == TD_MQTT_OFF || st.state == TD_MQTT_RETRY) return st.state;
        if (td_proto_millis() - start > timeout_ms) return st.state;
        td_proto_sleep_ms(20);
    }
}

static void print_msg(const td_mqtt_msg_t *m)
{
    printf("%s %s%s%s  %s%s\n", m->outgoing ? "->" : "<-", m->topic, m->retain ? " (retained)" : "",
           m->qos ? " qos1" : "", m->payload, m->len > TD_MQTT_PAYLOAD_KEEP ? "..." : "");
}

/* --------------------------------------------------------------- mqtt */

static void mqtt_usage(void)
{
    printf("usage:\n"
           "  mqtt connect                     use ~/mqtt.conf\n"
           "  mqtt connect -c <file>           use another config file\n"
           "  mqtt connect <broker> [options]  broker: host[:port], mqtt://host[:port],\n"
           "                                   mqtts://host[:port] (TLS, port 8883)\n"
           "      -u user  -P password  -i client-id  -k keepalive  -r (reconnect)\n"
           "      --tls  --cafile f  --cert f  --key f  --keypass p  --insecure\n"
           "  mqtt config init | show [file]   write a template / show the settings\n"
           "  mqtt disconnect | status\n"
           "  mqtt sub [-q 0|1] <topic>        (wildcards + and # allowed)\n"
           "  mqtt unsub <topic>\n"
           "  mqtt pub [-q 0|1] [-r] <topic> <message...>   (-r: retain)\n"
           "  mqtt log [count]                 last messages in and out\n"
           "  mqtt listen [seconds]            print messages as they arrive (default 30)\n");
}

#define DEFAULT_CONF "~/mqtt.conf"

/* Config-file paths are resolved like any shell path: ~, the current
 * folder, and the user's sandbox. */
static bool shell_resolve(const char *path, char *real, size_t cap, void *ctx)
{
    tdsh_session_t *session = ctx;
    return tdsh_path_to_real(session, path, real, cap, NULL, 0) == 0;
}

static bool conf_exists(tdsh_session_t *session, const char *path)
{
    char real[TD_MQTT_PATH_MAX];
    if (!shell_resolve(path, real, sizeof(real), session)) return false;
    FILE *f = fopen(real, "r");
    if (f) fclose(f);
    return f != NULL;
}

/* A real path as the shell shows it (the filesystem root removed). */
static const char *shown(const char *real)
{
    const tdsh_core_config_t *uc = tdsh_core_config();
    size_t n = uc && uc->fs_root ? strlen(uc->fs_root) : 0;
    if (n && strncmp(real, uc->fs_root, n) == 0 && real[n] == '/') return real + n;
    return real;
}

static void print_config(const td_mqtt_config_t *c)
{
    printf("broker     %s://%s:%u\n", c->tls ? "mqtts" : "mqtt", c->host,
           (unsigned)(c->port ? c->port : c->tls ? TD_MQTT_TLS_PORT : TD_MQTT_DEFAULT_PORT));
    if (c->client_id[0]) printf("client_id  %s\n", c->client_id);
    if (c->user[0]) printf("username   %s  (password %s)\n", c->user, c->pass[0] ? "set" : "none");
    if (c->tls) {
        printf("cafile     %s\n", c->ca_file[0] ? shown(c->ca_file) : "(the device's trusted roots)");
        if (c->cert_file[0]) printf("certfile   %s\nkeyfile    %s%s\n", shown(c->cert_file), shown(c->key_file),
                                    c->key_pass[0] ? "  (with password)" : "");
        if (c->insecure) printf("WARNING    tls_insecure: the server certificate is not checked\n");
    }
    printf("keepalive  %u s, reconnect %s, clean_session %s\n", (unsigned)(c->keepalive ? c->keepalive : 60),
           c->auto_reconnect ? "on" : "off", c->persistent ? "off" : "on");
    if (c->will_topic[0]) printf("will       %s = %s (qos %u%s)\n", c->will_topic, c->will_payload,
                                 (unsigned)c->will_qos, c->will_retain ? ", retained" : "");
    for (int i = 0; i < c->nsubs; i++) printf("subscribe  %s (qos %u)\n", c->sub_topic[i], (unsigned)c->sub_qos[i]);
}

static int mqtt_config_cmd(tdsh_session_t *session, int argc, char **argv)
{
    const char *op = argc > 2 ? argv[2] : "show";
    const char *file = argc > 3 ? argv[3] : DEFAULT_CONF;
    if (!strcmp(op, "init")) {
        char real[TD_MQTT_PATH_MAX];
        if (!shell_resolve(file, real, sizeof(real), session)) {
            printf("mqtt: %s is not allowed\n", file);
            return 1;
        }
        if (conf_exists(session, file)) {
            printf("mqtt: %s already exists (edit it with nano)\n", file);
            return 1;
        }
        FILE *f = fopen(real, "w");
        if (!f || fputs(td_mqtt_config_template, f) < 0) {
            if (f) fclose(f);
            printf("mqtt: cannot write %s\n", file);
            return 1;
        }
        fclose(f);
        printf("Wrote %s. Edit it (nano %s), then run: mqtt connect\n", file, file);
        return 0;
    }
    if (!strcmp(op, "show")) {
        td_mqtt_config_t *c = malloc(sizeof(*c));
        char err[96];
        if (!c) return 1;
        bool ok = td_mqtt_load_config(file, shell_resolve, session, c, err, sizeof(err));
        if (ok) print_config(c);
        else printf("mqtt: %s: %s\n", file, err);
        memset(c, 0, sizeof(*c));
        free(c);
        return ok ? 0 : 1;
    }
    mqtt_usage();
    return 2;
}

static int mqtt_connect_cmd(tdsh_session_t *session, int argc, char **argv)
{
    td_mqtt_config_t *cfg = calloc(1, sizeof(*cfg));     /* ~1.7 KB: not on the stack */
    if (!cfg) return 1;
    int rc = 2;
    char err[160];
    const char *broker = NULL, *conf = NULL;
    bool bad = false;

    /* A config file first (-c, or ~/mqtt.conf when no broker is given);
     * options on the command line then override it. */
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "-c") && i + 1 < argc) conf = argv[i + 1];
    bool has_broker = false;
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] != '-') {
            has_broker = true;
            break;
        }
        if (strcmp(argv[i], "-r") && strcmp(argv[i], "--tls") && strcmp(argv[i], "--insecure")) i++;   /* skip value */
    }
    if (!conf && !has_broker) {
        if (!conf_exists(session, DEFAULT_CONF)) {
            printf("mqtt: no broker given and no %s (create one with: mqtt config init)\n", DEFAULT_CONF);
            goto out;
        }
        conf = DEFAULT_CONF;
    }
    if (conf && !td_mqtt_load_config(conf, shell_resolve, session, cfg, err, sizeof(err))) {
        printf("mqtt: %s: %s\n", conf, err);
        rc = 1;
        goto out;
    }

    for (int i = 2; i < argc && !bad; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "-c") && v) i++;
        else if (!strcmp(a, "-u") && v) snprintf(cfg->user, sizeof(cfg->user), "%s", argv[++i]);
        else if (!strcmp(a, "-P") && v) snprintf(cfg->pass, sizeof(cfg->pass), "%s", argv[++i]);
        else if (!strcmp(a, "-i") && v) snprintf(cfg->client_id, sizeof(cfg->client_id), "%s", argv[++i]);
        else if (!strcmp(a, "-k") && v) cfg->keepalive = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(a, "-r")) cfg->auto_reconnect = true;
        else if (!strcmp(a, "--tls")) cfg->tls = true;
        else if (!strcmp(a, "--insecure")) cfg->insecure = cfg->tls = true;
        else if (!strcmp(a, "--keypass") && v) snprintf(cfg->key_pass, sizeof(cfg->key_pass), "%s", argv[++i]);
        else if ((!strcmp(a, "--cafile") || !strcmp(a, "--cert") || !strcmp(a, "--key")) && v) {
            char *dst = !strcmp(a, "--cafile") ? cfg->ca_file : !strcmp(a, "--cert") ? cfg->cert_file : cfg->key_file;
            if (!shell_resolve(argv[++i], dst, TD_MQTT_PATH_MAX, session)) {
                printf("mqtt: %s is not allowed\n", argv[i]);
                bad = true;
            }
            cfg->tls = true;
        } else if (a[0] != '-' && !broker) {
            broker = a;
            if (!td_mqtt_parse_broker(broker, cfg)) {
                printf("mqtt: bad broker %s\n", broker);
                bad = true;
            }
        } else {
            mqtt_usage();
            bad = true;
        }
    }
    if (bad) goto out;

    uint16_t port = cfg->port ? cfg->port : cfg->tls ? TD_MQTT_TLS_PORT : TD_MQTT_DEFAULT_PORT;
    printf("Connecting to %s://%s:%u%s...\n", cfg->tls ? "mqtts" : "mqtt", cfg->host, (unsigned)port,
           conf ? " (settings from the config file)" : "");
    fflush(stdout);
    if (!td_mqtt_connect(cfg, err, sizeof(err))) {
        printf("mqtt: %s\n", err);
        rc = 1;
        goto out;
    }
    td_mqtt_state_t s = mqtt_wait(15000);
    td_mqtt_status_t st;
    td_mqtt_status(&st);
    printf("%s\n", st.text);
    if (st.security[0]) printf("Security: %s\n", st.security);
    rc = s == TD_MQTT_CONNECTED ? 0 : 1;
out:
    memset(cfg, 0, sizeof(*cfg));                     /* passwords */
    free(cfg);
    return rc;
}

static int mqtt_listen(int seconds)
{
    uint32_t seen = td_mqtt_last_seq();
    uint32_t start = td_proto_millis();
    printf("Listening for %d s...\n", seconds);
    fflush(stdout);
    int got = 0;
    while (td_proto_millis() - start < (uint32_t)seconds * 1000u) {
        td_mqtt_poll();
        uint32_t last = td_mqtt_last_seq();
        if (last < seen) seen = 0;                  /* reconnected */
        while (seen < last) {
            td_mqtt_msg_t m;
            if (td_mqtt_message(++seen, &m) && !m.outgoing) {
                print_msg(&m);
                got++;
            }
        }
        fflush(stdout);
        td_proto_sleep_ms(50);
    }
    printf("%d message%s\n", got, got == 1 ? "" : "s");
    return 0;
}

static int cmd_mqtt(tdsh_session_t *session, int argc, char **argv)
{
    if (argc < 2) {
        mqtt_usage();
        return 2;
    }
    const char *sub = argv[1];
    if (!strcmp(sub, "connect")) return mqtt_connect_cmd(session, argc, argv);
    if (!strcmp(sub, "config")) return mqtt_config_cmd(session, argc, argv);
    if (!strcmp(sub, "disconnect")) {
        td_mqtt_disconnect();
        printf("Disconnected\n");
        return 0;
    }
    if (!strcmp(sub, "status")) {
        td_mqtt_status_t st;
        td_mqtt_status(&st);
        printf("MQTT: %s\n", state_name(st.state));
        if (st.broker[0]) printf("Broker: %s\n", st.broker);
        printf("%s\n", st.text);
        if (st.security[0]) printf("Security: %s\n", st.security);
        if (st.state == TD_MQTT_CONNECTED)
            printf("Messages: %u received, %u published\n", (unsigned)st.rx, (unsigned)st.tx);
        int qos;
        char t[TD_MQTT_TOPIC_MAX];
        for (int i = 0; td_mqtt_subscription(i, t, sizeof(t), &qos); i++) printf("Subscribed: %s (qos %d)\n", t, qos);
        return 0;
    }
    if (!strcmp(sub, "sub") || !strcmp(sub, "unsub")) {
        int qos = 0, i = 2;
        if (i + 1 < argc && !strcmp(argv[i], "-q")) {
            qos = atoi(argv[i + 1]);
            i += 2;
        }
        if (i != argc - 1) {
            mqtt_usage();
            return 2;
        }
        int rc = !strcmp(sub, "sub") ? td_mqtt_subscribe(argv[i], qos) : td_mqtt_unsubscribe(argv[i]);
        if (rc == 0) {
            td_mqtt_status_t st;
            td_mqtt_status(&st);
            if (st.state != TD_MQTT_CONNECTED)
                printf("%s %s once connected\n", !strcmp(sub, "sub") ? "Will subscribe to" : "Forgot", argv[i]);
            else
                printf("%s %s\n", !strcmp(sub, "sub") ? "Subscribed to" : "Unsubscribed from", argv[i]);
            return 0;
        }
        printf("mqtt: %s\n", rc == -2 ? "too many subscriptions (8 at most)"
                             : !strcmp(sub, "sub") ? "connect first (mqtt connect), topic up to 63 characters"
                                                   : "not subscribed to that topic");
        return 1;
    }
    if (!strcmp(sub, "pub")) {
        int qos = 0, i = 2;
        bool retain = false;
        while (i < argc && argv[i][0] == '-' && argv[i][1]) {
            if (!strcmp(argv[i], "-r")) retain = true, i++;
            else if (!strcmp(argv[i], "-q") && i + 1 < argc) qos = atoi(argv[i + 1]), i += 2;
            else break;
        }
        if (i + 1 >= argc) {
            mqtt_usage();
            return 2;
        }
        const char *topic = argv[i++];
        char msg[512];
        msg[0] = '\0';
        for (; i < argc; i++) {                        /* the rest, joined by spaces */
            size_t used = strlen(msg);
            snprintf(msg + used, sizeof(msg) - used, "%s%s", used ? " " : "", argv[i]);
        }
        int rc = td_mqtt_publish(topic, msg, (int)strlen(msg), qos, retain);
        if (rc == 0) {
            td_mqtt_poll();
            printf("Published %u bytes to %s\n", (unsigned)strlen(msg), topic);
            return 0;
        }
        printf("mqtt: %s\n", rc == -2 ? "message too big or connection busy"
                                      : "connect first; topic without + or #, up to 63 characters");
        return 1;
    }
    if (!strcmp(sub, "log")) {
        int want = argc > 2 ? atoi(argv[2]) : 10;
        if (want < 1) want = 1;
        uint32_t last = td_mqtt_last_seq();
        if (!last) {
            printf("No messages yet\n");
            return 0;
        }
        uint32_t first = last > (uint32_t)want ? last - (uint32_t)want + 1 : 1;
        for (uint32_t s = first; s <= last; s++) {
            td_mqtt_msg_t m;
            if (td_mqtt_message(s, &m)) print_msg(&m);
        }
        return 0;
    }
    if (!strcmp(sub, "listen")) {
        int secs = argc > 2 ? atoi(argv[2]) : 30;
        if (secs < 1 || secs > 3600) secs = 30;
        td_mqtt_status_t st;
        td_mqtt_status(&st);
        if (st.state != TD_MQTT_CONNECTED) {
            printf("mqtt: not connected\n");
            return 1;
        }
        return mqtt_listen(secs);
    }
    mqtt_usage();
    return 2;
}

/* ------------------------------------------------------------- modbus */

static void modbus_usage(void)
{
    printf("usage:\n"
           "  modbus read  <device> <unit> <co|di|hr|ir> <addr> [count] [-i ms] [-n times]\n"
           "  modbus write <device> <unit> <co|hr> <addr> <value> [value...]\n"
           "  modbus server start [port] | stop | status\n"
           "  modbus server get <co|di|hr|ir> <addr> [count]\n"
           "  modbus server set <co|di|hr|ir> <addr> <value> [value...]\n"
           "device: host[:port] for Modbus TCP (port 502), or rtu[N][:baud[:8N1|8E1|8O1]]\n"
           "for Modbus RTU%s%s%s%s.\n"
           "co coils, di discrete inputs, hr holding registers, ir input registers;\n"
           "addresses start at 0; values in decimal or 0x hex.\n"
           "-i ms repeats the read every ms milliseconds (10..3600000, start to start),\n"
           "-n times reads that often (default 10; 0 = until Ctrl+C in the desktop's Terminal window).\n",
           td_mb_serial() ? ": rtu1 = " : " (not available here)", td_mb_serial() ? td_mb_serial()->name(1) : "",
           td_mb_serial() && td_mb_serial()->ports > 1 ? ", rtu2 = " : "",
           td_mb_serial() && td_mb_serial()->ports > 1 ? td_mb_serial()->name(2) : "");
}

static void print_values(td_mb_table_t t, uint16_t addr, int count, const uint16_t *v)
{
    bool bits = t == TD_MB_COILS || t == TD_MB_DISCRETE;
    for (int i = 0; i < count; i++) {
        if (bits) printf("  %5u  %u\n", (unsigned)(addr + i), (unsigned)v[i]);
        else printf("  %5u  %6u  0x%04X  %6d\n", (unsigned)(addr + i), (unsigned)v[i], (unsigned)v[i], (int)(int16_t)v[i]);
    }
}

/* modbus read ... -i ms [-n times]: one line per read, then a summary. */
static int modbus_repeat(const td_mb_request_t *r, uint32_t interval, uint32_t times)
{
    if (times)
        printf("Reading every %u ms, %u times%s\n", (unsigned)interval, (unsigned)times,
               s_break_check ? " (Ctrl+C in the desktop's Terminal stops it)" : "");
    else
        printf("Reading every %u ms until Ctrl+C%s\n", (unsigned)interval,
               s_break_check ? "" : " (not available here: use -n)");
    printf("%5s %8s %7s  values from address %u\n", "#", "time ms", "answer", (unsigned)r->addr);
    fflush(stdout);

    td_mb_result_t res;
    char err[80];
    uint32_t start = td_proto_millis(), next = start;
    uint32_t done = 0, ok = 0, rmin = 0xFFFFFFFFu, rmax = 0, rsum = 0;
    bool stopped = false;
    while (times == 0 || done < times) {
        /* Wait for the next start, watching for Ctrl+C. */
        while ((int32_t)(td_proto_millis() - next) < 0) {
            if (break_requested()) {
                stopped = true;
                break;
            }
            uint32_t left = next - td_proto_millis();
            td_proto_sleep_ms(left > 5 && left < 1000000u ? 5 : 1);
        }
        if (stopped || break_requested()) {
            stopped = true;
            break;
        }
        uint32_t t0 = td_proto_millis();
        next += interval;
        if ((int32_t)(t0 - next) >= 0) next = t0 + interval;   /* slower than the interval */
        done++;
        printf("%5u %8u ", (unsigned)done, (unsigned)(t0 - start));
        if (!td_mb_transact(r, &res, err, sizeof(err))) {
            printf("  --  %s\n", err);
        } else if (res.status != 0) {
            printf("  --  %s\n", res.text);
        } else {
            ok++;
            rsum += res.ms;
            if (res.ms < rmin) rmin = res.ms;
            if (res.ms > rmax) rmax = res.ms;
            printf("%4u ms ", (unsigned)res.ms);
            for (int i = 0; i < res.count; i++) printf(" %u", (unsigned)res.values[i]);
            printf("\n");
        }
        fflush(stdout);
    }
    uint32_t took = td_proto_millis() - start;
    if (stopped) printf("^C\n");
    printf("%u read%s, %u ok, %u failed", (unsigned)done, done == 1 ? "" : "s", (unsigned)ok, (unsigned)(done - ok));
    if (done > 1) printf("; one every %u ms", (unsigned)(took / (done - 1)));
    if (ok) printf("; answer %u/%u/%u ms (min/avg/max)", (unsigned)rmin, (unsigned)(rsum / ok), (unsigned)rmax);
    printf("\n");
    return ok == done ? 0 : 1;
}

static int modbus_client(int argc, char **argv, bool write)
{
    /* -i ms / -n times (read only) may stand anywhere after "read". */
    uint32_t interval = 0, times = 10;
    bool have_times = false;
    int kept = 2;
    for (int i = 2; i < argc; i++) {
        bool is_i = !strcmp(argv[i], "-i"), is_n = !strcmp(argv[i], "-n");
        if (!write && (is_i || is_n)) {
            char *end = NULL;
            unsigned long v = i + 1 < argc ? strtoul(argv[i + 1], &end, 10) : 0;
            if (i + 1 >= argc || !argv[i + 1][0] || *end) {
                printf("modbus: %s needs a number\n", argv[i]);
                return 2;
            }
            if (is_i && (v < 10 || v > 3600000ul)) {
                printf("modbus: -i must be 10..3600000 ms\n");
                return 2;
            }
            if (is_n && v > 1000000ul) {
                printf("modbus: -n must be 0..1000000\n");
                return 2;
            }
            if (is_i) interval = (uint32_t)v;
            else {
                times = (uint32_t)v;
                have_times = true;
            }
            i++;
            continue;
        }
        argv[kept++] = argv[i];
    }
    argc = kept;
    if (have_times && !interval) {
        printf("modbus: -n goes with -i (the interval)\n");
        return 2;
    }
    if (interval && have_times && times == 0 && !s_break_check) {
        printf("modbus: -n 0 needs Ctrl+C, which is not available here: give a count\n");
        return 2;
    }

    if (argc < 6 + (write ? 1 : 0)) {
        modbus_usage();
        return 2;
    }
    td_mb_request_t r;
    memset(&r, 0, sizeof(r));
    snprintf(r.target, sizeof(r.target), "%s", argv[2]);
    uint16_t unit;
    td_mb_table_t t;
    if (!parse_u16(argv[3], &unit) || unit > 255) {
        printf("modbus: unit must be 0..255\n");
        return 2;
    }
    if (!td_mb_parse_table(argv[4], &t)) {
        printf("modbus: table must be co, di, hr or ir\n");
        return 2;
    }
    if (!parse_u16(argv[5], &r.addr)) {
        printf("modbus: bad address %s\n", argv[5]);
        return 2;
    }
    r.unit = (uint8_t)unit;
    if (write) {
        if (t != TD_MB_COILS && t != TD_MB_HOLDING) {
            printf("modbus: only coils (co) and holding registers (hr) can be written\n");
            return 2;
        }
        for (int i = 6; i < argc; i++) {
            if (r.nvalues >= TD_MB_MAX_WRITE || !parse_u16(argv[i], &r.values[r.nvalues])) {
                printf("modbus: bad value %s (up to %d values)\n", argv[i], TD_MB_MAX_WRITE);
                return 2;
            }
            r.nvalues++;
        }
        if (t == TD_MB_COILS) r.fc = r.nvalues == 1 ? 5 : 15;
        else r.fc = r.nvalues == 1 ? 6 : 16;
    } else {
        uint16_t count = 1;
        if (argc > 6 && (!parse_u16(argv[6], &count) || count < 1 || count > TD_MB_MAX_READ)) {
            printf("modbus: count must be 1..%d\n", TD_MB_MAX_READ);
            return 2;
        }
        r.fc = (uint8_t)t;
        r.count = count;
        if (interval) return modbus_repeat(&r, interval, times);
    }
    td_mb_result_t res;               /* on the shell task's stack, not in RAM for good */
    char err[80];
    if (!td_mb_transact(&r, &res, err, sizeof(err))) {
        printf("modbus: %s\n", err);
        return 1;
    }
    printf("%s (%u ms)\n", res.text, (unsigned)res.ms);
    if (res.status == 0 && !write) print_values(t, res.addr, res.count, res.values);
    return res.status == 0 ? 0 : 1;
}

static int modbus_server(int argc, char **argv)
{
    const char *op = argc > 2 ? argv[2] : "status";
    char err[80];
    if (!strcmp(op, "start")) {
        uint16_t port = TD_MB_TCP_PORT;
        if (argc > 3 && (!parse_u16(argv[3], &port) || !port)) {
            printf("modbus: bad port\n");
            return 2;
        }
        if (!td_mb_server_start(port, err, sizeof(err))) {
            printf("modbus: %s\n", err);
            return 1;
        }
        printf("Modbus TCP server listening on port %u (any unit id; %d of each table)\n", (unsigned)port,
               TD_MB_TABLE_SIZE);
        return 0;
    }
    if (!strcmp(op, "stop")) {
        td_mb_server_stop();
        printf("Modbus TCP server stopped\n");
        return 0;
    }
    if (!strcmp(op, "status")) {
        uint16_t port;
        int clients;
        uint32_t reqs;
        if (td_mb_server_status(&port, &clients, &reqs))
            printf("Modbus TCP server: running on port %u, %d client%s, %u requests\n", (unsigned)port, clients,
                   clients == 1 ? "" : "s", (unsigned)reqs);
        else
            printf("Modbus TCP server: stopped\n");
        return 0;
    }
    if (!strcmp(op, "get") || !strcmp(op, "set")) {
        bool set = !strcmp(op, "set");
        td_mb_table_t t;
        uint16_t addr;
        if (argc < 5 || !td_mb_parse_table(argv[3], &t) || !parse_u16(argv[4], &addr)) {
            modbus_usage();
            return 2;
        }
        if (!td_mb_server_status(NULL, NULL, NULL)) {
            printf("modbus: start the server first (modbus server start)\n");
            return 1;
        }
        uint16_t v[TD_MB_MAX_WRITE];
        int n = 0;
        if (set) {
            for (int i = 5; i < argc; i++) {
                if (n >= TD_MB_MAX_WRITE || !parse_u16(argv[i], &v[n])) {
                    printf("modbus: bad value %s\n", argv[i]);
                    return 2;
                }
                n++;
            }
            if (!n) {
                modbus_usage();
                return 2;
            }
            int done = td_mb_server_set(t, addr, (uint16_t)n, v);
            printf("Set %d %s from address %u\n", done, td_mb_table_name(t), (unsigned)addr);
            return done == n ? 0 : 1;
        }
        uint16_t count = 1;
        if (argc > 5 && (!parse_u16(argv[5], &count) || count < 1 || count > TD_MB_MAX_WRITE)) {
            printf("modbus: count must be 1..%d\n", TD_MB_MAX_WRITE);
            return 2;
        }
        n = td_mb_server_get(t, addr, count, v);
        if (n == 0) {
            printf("modbus: addresses go up to %d\n", TD_MB_TABLE_SIZE - 1);
            return 1;
        }
        print_values(t, addr, n, v);
        return 0;
    }
    modbus_usage();
    return 2;
}

static int cmd_modbus(tdsh_session_t *session, int argc, char **argv)
{
    (void)session;
    if (argc < 2) {
        modbus_usage();
        return 2;
    }
    if (!strcmp(argv[1], "read")) return modbus_client(argc, argv, false);
    if (!strcmp(argv[1], "write")) return modbus_client(argc, argv, true);
    if (!strcmp(argv[1], "server")) return modbus_server(argc, argv);
    modbus_usage();
    return 2;
}

/* ---------------------------------------------------------- register */

static const tdsh_command_t s_cmds[] = {
    { "mqtt", "mqtt <connect|config|disconnect|status|sub|unsub|pub|log|listen> ...",
      "MQTT client, TLS and ~/mqtt.conf (shared with the MQTT app)", cmd_mqtt, 0 },
    { "modbus", "modbus <read|write|server> ...",
      "Modbus TCP/RTU client and TCP server (shared with the Modbus app)", cmd_modbus, 0 },
};

int td_proto_register_shell_commands(void)
{
    return tdsh_register_commands(s_cmds, sizeof(s_cmds) / sizeof(s_cmds[0]));
}
