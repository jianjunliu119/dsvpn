#include "os.h"
#include "dsvpn.h"

#ifdef __linux__
int tun_create(char if_name[IFNAMSIZ], const char *wanted_name)
{
    struct ifreq ifr;
    int          fd;
    int          err;

    fd = open("/dev/net/tun", O_RDWR);
    if (fd == -1) {
        return -1;
    }
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", wanted_name == NULL ? "" : wanted_name);
    if (ioctl(fd, TUNSETIFF, &ifr) != 0) {
        err = errno;
        (void) close(fd);
        errno = err;
        return -1;
    }
    snprintf(if_name, IFNAMSIZ, "%s", ifr.ifr_name);

    return fd;
}
#elif defined(__APPLE__)
static int tun_create_by_id(char if_name[IFNAMSIZ], unsigned int id)
{
    struct ctl_info     ci;
    struct sockaddr_ctl sc;
    int                 err;
    int                 fd;

    if ((fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL)) == -1) {
        return -1;
    }
    memset(&ci, 0, sizeof ci);
    snprintf(ci.ctl_name, sizeof ci.ctl_name, "%s", UTUN_CONTROL_NAME);
    if (ioctl(fd, CTLIOCGINFO, &ci)) {
        err = errno;
        (void) close(fd);
        errno = err;
        return -1;
    }
    memset(&sc, 0, sizeof sc);
    sc = (struct sockaddr_ctl){
        .sc_id      = ci.ctl_id,
        .sc_len     = sizeof sc,
        .sc_family  = AF_SYSTEM,
        .ss_sysaddr = AF_SYS_CONTROL,
        .sc_unit    = id + 1,
    };
    if (connect(fd, (struct sockaddr *) &sc, sizeof sc) != 0) {
        err = errno;
        (void) close(fd);
        errno = err;
        return -1;
    }
    snprintf(if_name, IFNAMSIZ, "utun%u", id);

    return fd;
}

int tun_create(char if_name[IFNAMSIZ], const char *wanted_name)
{
    unsigned int id;
    int          fd;

    if (wanted_name == NULL || *wanted_name == 0) {
        for (id = 0; id < 32; id++) {
            if ((fd = tun_create_by_id(if_name, id)) != -1) {
                return fd;
            }
        }
        return -1;
    }
    if (sscanf(wanted_name, "utun%u", &id) != 1) {
        errno = EINVAL;
        return -1;
    }
    return tun_create_by_id(if_name, id);
}
#else
int tun_create(char if_name[IFNAMSIZ], const char *wanted_name)
{
    char path[64];

    if (wanted_name == NULL) {
        fprintf(stderr,
                "The tunnel device name must be specified on that platform "
                "(try 'tun0')\n");
        errno = EINVAL;
        return -1;
    }
    snprintf(if_name, IFNAMSIZ, "%s", wanted_name);
    snprintf(path, sizeof path, "/dev/%s", wanted_name);

    return open(path, O_RDWR);
}
#endif

int tun_set_mtu(const char *if_name, int mtu)
{
    struct ifreq ifr;
    int          fd;

    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        return -1;
    }
    ifr.ifr_mtu = mtu;
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", if_name);
    if (ioctl(fd, SIOCSIFMTU, &ifr) != 0) {
        return -1;
    }
    return close(fd);
}

#ifdef __linux__
ssize_t tun_read(int fd, void *data, size_t size)
{
    return safe_read_partial(fd, data, size);
}

ssize_t tun_write(int fd, const void *data, size_t size)
{
    return safe_write(fd, data, size, TIMEOUT);
}
#elif defined(__APPLE__) || defined(__OpenBSD__) || defined(__FreeBSD__)
ssize_t tun_read(int fd, void *data, size_t size)
{
    ssize_t  ret;
    uint32_t family;

    struct iovec iov[2] = {
        {
            .iov_base = &family,
            .iov_len  = sizeof family,
        },
        {
            .iov_base = data,
            .iov_len  = size,
        },
    };

    ret = readv(fd, iov, 2);
    if (ret <= (ssize_t) 0) {
        return -1;
    }
    if (ret <= (ssize_t) sizeof family) {
        return 0;
    }
    return ret - sizeof family;
}

ssize_t tun_write(int fd, const void *data, size_t size)
{
    uint32_t family;
    ssize_t  ret;

    if (size < 20) {
        return 0;
    }
    switch (*(const uint8_t *) data >> 4) {
    case 4:
        family = htonl(AF_INET);
        break;
    case 6:
        family = htonl(AF_INET6);
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    struct iovec iov[2] = {
        {
            .iov_base = &family,
            .iov_len  = sizeof family,
        },
        {
            .iov_base = (void *) data,
            .iov_len  = size,
        },
    };
    ret = writev(fd, iov, 2);
    if (ret <= (ssize_t) 0) {
        return ret;
    }
    if (ret <= (ssize_t) sizeof family) {
        return 0;
    }
    return ret - sizeof family;
}
#endif

static char *read_from_shell_command(char *result, size_t sizeof_result, const char *command)
{
    FILE *fp;
    char *pnt;

    if ((fp = popen(command, "r")) == NULL) {
        return NULL;
    }
    if (fgets(result, sizeof_result, fp) == NULL) {
        fclose(fp);
        fprintf(stderr, "Command [%s] failed]\n", command);
        return NULL;
    }
    if ((pnt = strrchr(result, '\n')) != NULL) {
        *pnt = 0;
    }
    (void) pclose(fp);

    return *result == 0 ? NULL : result;
}

const char *get_default_gw_ip(void)
{
    static char gw[64];
#ifdef __APPLE__
    return read_from_shell_command(
        gw, sizeof gw, "route -n get default 2>/dev/null|awk '/gateway/{print $2}'|head -n1");
#elif defined(__linux__)
    return read_from_shell_command(gw, sizeof gw,
                                   "ip route show default 2>/dev/null|awk '/default/{print $3}'");
#elif defined(__OpenBSD__) || defined(__FreeBSD__)
    return read_from_shell_command(gw, sizeof gw, "netstat -rn|awk '/^default/{print $2}'");
#else
    return NULL;
#endif
}

const char *get_default_ext_if_name(void)
{
    static char if_name[64];
#ifdef __APPLE__
    return read_from_shell_command(if_name, sizeof if_name,
                                   "route -n get default 2>/dev/null|awk "
                                   "'/interface/{print $2}'|head -n1");
#elif defined(__linux__)
    return read_from_shell_command(if_name, sizeof if_name,
                                   "ip route show default 2>/dev/null|awk '/default/{print $5}'");
#elif defined(__OpenBSD__) || defined(__FreeBSD__)
    return read_from_shell_command(if_name, sizeof if_name,
                                   "netstat -rn|awk '/^default/{print $8}'");
#else
    return NULL;
#endif
}

int tcp_opts(int fd)
{
    int on = 1;

    (void) setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (char *) &on, sizeof on);
#ifdef TCP_QUICKACK
    (void) setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, (char *) &on, sizeof on);
#else
    (void) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *) &on, sizeof on);
#endif
#ifdef TCP_CONGESTION
    (void) setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, OUTER_CONGESTION_CONTROL_ALG,
                      sizeof OUTER_CONGESTION_CONTROL_ALG - 1);
#endif
#if BUFFERBLOAT_CONTROL && defined(TCP_NOTSENT_LOWAT)
    {
        unsigned int notsent_lowat = NOTSENT_LOWAT;
        (void) setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, (char *) &notsent_lowat,
                          sizeof notsent_lowat);
    }
#endif
    return 0;
}

int shell_cmd(const char *substs[][2], const char *args_str)
{
    char * args[64];
    char   cmdbuf[4096];
    pid_t  child;
    size_t args_i = 0, cmdbuf_i = 0, args_str_i, i;
    int    c, exit_status, is_space = 1;

    errno = ENOSPC;
    for (args_str_i = 0; (c = args_str[args_str_i]) != 0; args_str_i++) {
        if (isspace((unsigned char) c)) {
            if (!is_space) {
                if (cmdbuf_i >= sizeof cmdbuf) {
                    return -1;
                }
                cmdbuf[cmdbuf_i++] = 0;
            }
            is_space = 1;
            continue;
        }
        if (is_space) {
            if (args_i >= sizeof args / sizeof args[0]) {
                return -1;
            }
            args[args_i++] = &cmdbuf[cmdbuf_i];
        }
        is_space = 0;
        for (i = 0; substs[i][0] != NULL; i++) {
            size_t pat_len = strlen(substs[i][0]), sub_len;
            if (!strncmp(substs[i][0], &args_str[args_str_i], pat_len)) {
                sub_len = strlen(substs[i][1]);
                if (sizeof cmdbuf - cmdbuf_i <= sub_len) {
                    return -1;
                }
                memcpy(&cmdbuf[cmdbuf_i], substs[i][1], sub_len);
                args_str_i += pat_len - 1;
                cmdbuf_i += sub_len;
                break;
            }
        }
        if (substs[i][0] == NULL) {
            if (cmdbuf_i >= sizeof cmdbuf) {
                return -1;
            }
            cmdbuf[cmdbuf_i++] = c;
        }
    }
    if (!is_space) {
        if (cmdbuf_i >= sizeof cmdbuf) {
            return -1;
        }
        cmdbuf[cmdbuf_i++] = 0;
    }
    if (args_i >= sizeof args / sizeof args[0]) {
        return -1;
    }
    args[args_i] = NULL;
    if ((child = vfork()) == (pid_t) -1) {
        return -1;
    } else if (child == (pid_t) 0) {
        execvp(args[0], args);
        _exit(1);
    } else if (waitpid(child, &exit_status, 0) == (pid_t) -1 || !WIFEXITED(exit_status)) {
        return -1;
    }
    return 0;
}

Cmds firewall_rules_cmds(int is_server)
{
    if (is_server) {
#ifdef __linux__
        static const char *set_cmds
            []   = { "sysctl net.ipv4.ip_forward=1",
                   "ip addr add $LOCAL_TUN_IP peer $REMOTE_TUN_IP dev $IF_NAME",
                   "ip link set dev $IF_NAME up",
                   "iptables -t nat -A POSTROUTING -o $EXT_IF_NAME -s $REMOTE_TUN_IP -j MASQUERADE",
                   "iptables -t filter -A FORWARD -i $EXT_IF_NAME -o $IF_NAME -m state --state "
                   "RELATED,ESTABLISHED -j ACCEPT",
                   "iptables -t filter -A FORWARD -i $IF_NAME -o $EXT_IF_NAME -j ACCEPT",
                   NULL },
   *unset_cmds[] = {
       "iptables -t nat -D POSTROUTING -o $EXT_IF_NAME -s $REMOTE_TUN_IP -j MASQUERADE",
       "iptables -t filter -D FORWARD -i $EXT_IF_NAME -o $IF_NAME -m state --state "
       "RELATED,ESTABLISHED -j ACCEPT",
       "iptables -t filter -D FORWARD -i $IF_NAME -o $EXT_IF_NAME -j ACCEPT", NULL
   };
#else
        static const char *const *set_cmds = NULL, *const *unset_cmds = NULL;
#endif
        return (Cmds){ set_cmds, unset_cmds };
    } else {
#if defined(__APPLE__) || defined(__OpenBSD__) || defined(__FreeBSD__)
        static const char *set_cmds
            []   = { "ifconfig $IF_NAME $LOCAL_TUN_IP $REMOTE_TUN_IP up",
                   "ifconfig $IF_NAME inet6 $LOCAL_TUN_IP6 $REMOTE_TUN_IP6 prefixlen 128 up",
                   "route add $EXT_IP $EXT_GW_IP",
                   "route add 0/1 $REMOTE_TUN_IP",
                   "route add 128/1 $REMOTE_TUN_IP",
                   "route add -inet6 -blackhole 0000::/1 $REMOTE_TUN_IP6",
                   "route add -inet6 -blackhole 8000::/1 $REMOTE_TUN_IP6",
                   NULL },
   *unset_cmds[] = { "route delete $EXT_IP $EXT_GW_IP", NULL };
#elif defined(__linux__)
        static const char
            *set_cmds[]   = { "sysctl net.ipv4.tcp_congestion_control=bbr",
                            "ip link set dev $IF_NAME up",
                            "ip addr add $LOCAL_TUN_IP peer $REMOTE_TUN_IP dev $IF_NAME",
                            "ip -6 addr add $LOCAL_TUN_IP6 peer $REMOTE_TUN_IP6 dev $IF_NAME",
                            "ip route add $EXT_IP via $EXT_GW_IP",
                            "ip route add 0/1 via $REMOTE_TUN_IP",
                            "ip route add 128/1 via $REMOTE_TUN_IP",
                            "ip -6 route add 0000::/1 via $REMOTE_TUN_IP6",
                            "ip -6 route add 8000::/1 via $REMOTE_TUN_IP6",
                            NULL },
            *unset_cmds[] = { "ip route del $EXT_IP via $EXT_GW_IP", NULL };
#else
        static const char *const *set_cmds = NULL, *const *unset_cmds = NULL;
#endif
        return (Cmds){ set_cmds, unset_cmds };
    }
}
