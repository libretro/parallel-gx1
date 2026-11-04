/* curl_config.h for the static Makefile build: the per-platform CMake configure results (Windows: MinGW-w64, Android: NDK, Linux: glibc) merged into one header. The Apple values are the Linux ones with the Darwin differences applied and were not produced by a configure run. */
#ifndef CURL_CONFIG_H_MAKEFILE
#define CURL_CONFIG_H_MAKEFILE

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if defined(_WIN32)
/* #undef CURL_CA_BUNDLE */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define CURL_CA_BUNDLE "/etc/ssl/cert.pem"
#elif defined(__APPLE__)
/* #undef CURL_CA_BUNDLE */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef CURL_CA_BUNDLE */
#elif defined(__ANDROID__)
/* #undef CURL_CA_BUNDLE */
#else
#define CURL_CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"
#endif
/* #undef CURL_CA_FALLBACK */
#if defined(_WIN32)
/* #undef CURL_CA_PATH */
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef CURL_CA_PATH */
#elif defined(__APPLE__)
/* #undef CURL_CA_PATH */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define CURL_CA_PATH "/system/etc/security/cacerts"
#elif defined(__ANDROID__)
#define CURL_CA_PATH "/system/etc/security/cacerts"
#else
#define CURL_CA_PATH "/etc/ssl/certs"
#endif
/* #undef CURL_DEFAULT_SSL_BACKEND */
/* #undef CURL_DISABLE_ALTSVC */
/* #undef CURL_DISABLE_COOKIES */
/* #undef CURL_DISABLE_BASIC_AUTH */
/* #undef CURL_DISABLE_BEARER_AUTH */
/* #undef CURL_DISABLE_DIGEST_AUTH */
/* #undef CURL_DISABLE_KERBEROS_AUTH */
/* #undef CURL_DISABLE_NEGOTIATE_AUTH */
/* #undef CURL_DISABLE_AWS */
#define CURL_DISABLE_DICT 1
/* #undef CURL_DISABLE_DOH */
#define CURL_DISABLE_FILE 1
/* #undef CURL_DISABLE_FORM_API */
#define CURL_DISABLE_FTP 1
/* #undef CURL_DISABLE_GETOPTIONS */
#define CURL_DISABLE_GOPHER 1
/* #undef CURL_DISABLE_HEADERS_API */
#if defined(_WIN32)
/* #undef CURL_DISABLE_HSTS */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define CURL_DISABLE_HSTS 1
#elif defined(__APPLE__)
#define CURL_DISABLE_HSTS 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define CURL_DISABLE_HSTS 1
#elif defined(__ANDROID__)
#define CURL_DISABLE_HSTS 1
#else
#define CURL_DISABLE_HSTS 1
#endif
/* #undef CURL_DISABLE_HTTP */
/* #undef CURL_DISABLE_HTTP_AUTH */
#define CURL_DISABLE_IMAP 1
#define CURL_DISABLE_LDAP 1
#define CURL_DISABLE_LDAPS 1
/* #undef CURL_DISABLE_LIBCURL_OPTION */
/* #undef CURL_DISABLE_MIME */
/* #undef CURL_DISABLE_BINDLOCAL */
#define CURL_DISABLE_MQTT 1
/* #undef CURL_DISABLE_NETRC */
/* #undef CURL_DISABLE_NTLM */
/* #undef CURL_DISABLE_PARSEDATE */
#define CURL_DISABLE_POP3 1
/* #undef CURL_DISABLE_PROGRESS_METER */
/* #undef CURL_DISABLE_PROXY */
#define CURL_DISABLE_IPFS 1
#define CURL_DISABLE_RTSP 1
/* #undef CURL_DISABLE_SHA512_256 */
/* #undef CURL_DISABLE_SHUFFLE_DNS */
#define CURL_DISABLE_SMB 1
#define CURL_DISABLE_SMTP 1
/* #undef CURL_DISABLE_WEBSOCKETS */
/* #undef CURL_DISABLE_SOCKETPAIR */
#define CURL_DISABLE_TELNET 1
#define CURL_DISABLE_TFTP 1
/* #undef CURL_DISABLE_VERBOSE_STRINGS */
/* #undef CURL_DISABLE_CA_SEARCH */
/* #undef CURL_CA_SEARCH_SAFE */
#define CURL_EXTERN_SYMBOL __attribute__((__visibility__("default")))
#if defined(_WIN32)
#define USE_WIN32_CRYPTO 1
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef USE_WIN32_CRYPTO */
#elif defined(__APPLE__)
/* #undef USE_WIN32_CRYPTO */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef USE_WIN32_CRYPTO */
#elif defined(__ANDROID__)
/* #undef USE_WIN32_CRYPTO */
#else
/* #undef USE_WIN32_CRYPTO */
#endif
/* #undef USE_WIN32_LDAP */
#define USE_IPV6 1
#if defined(_WIN32)
/* #undef HAVE_ALARM */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_ALARM 1
#elif defined(__APPLE__)
#define HAVE_ALARM 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_ALARM 1
#elif defined(__ANDROID__)
#define HAVE_ALARM 1
#else
#define HAVE_ALARM 1
#endif
/* #undef HAVE_ARC4RANDOM */
#if defined(_WIN32)
/* #undef HAVE_ARPA_INET_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_ARPA_INET_H 1
#elif defined(__APPLE__)
#define HAVE_ARPA_INET_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_ARPA_INET_H 1
#elif defined(__ANDROID__)
#define HAVE_ARPA_INET_H 1
#else
#define HAVE_ARPA_INET_H 1
#endif
#define HAVE_ATOMIC 1
#if defined(_WIN32)
/* #undef HAVE_ACCEPT4 */
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_ACCEPT4 */
#elif defined(__APPLE__)
/* #undef HAVE_ACCEPT4 */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_ACCEPT4 1
#elif defined(__ANDROID__)
#define HAVE_ACCEPT4 1
#else
#define HAVE_ACCEPT4 1
#endif
#if defined(_WIN32)
/* #undef HAVE_FNMATCH */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_FNMATCH 1
#elif defined(__APPLE__)
#define HAVE_FNMATCH 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_FNMATCH 1
#elif defined(__ANDROID__)
#define HAVE_FNMATCH 1
#else
#define HAVE_FNMATCH 1
#endif
#define HAVE_BASENAME 1
#define HAVE_BOOL_T 1
/* #undef HAVE_BUILTIN_AVAILABLE */
#if defined(_WIN32)
/* #undef HAVE_CLOCK_GETTIME_MONOTONIC */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_CLOCK_GETTIME_MONOTONIC 1
#elif defined(__APPLE__)
#define HAVE_CLOCK_GETTIME_MONOTONIC 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_CLOCK_GETTIME_MONOTONIC 1
#elif defined(__ANDROID__)
#define HAVE_CLOCK_GETTIME_MONOTONIC 1
#else
#define HAVE_CLOCK_GETTIME_MONOTONIC 1
#endif
#if defined(_WIN32)
/* #undef HAVE_CLOCK_GETTIME_MONOTONIC_RAW */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_CLOCK_GETTIME_MONOTONIC_RAW 1
#elif defined(__APPLE__)
#define HAVE_CLOCK_GETTIME_MONOTONIC_RAW 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_CLOCK_GETTIME_MONOTONIC_RAW 1
#elif defined(__ANDROID__)
#define HAVE_CLOCK_GETTIME_MONOTONIC_RAW 1
#else
#define HAVE_CLOCK_GETTIME_MONOTONIC_RAW 1
#endif
#if defined(_WIN32)
#define HAVE_CLOSESOCKET 1
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_CLOSESOCKET */
#elif defined(__APPLE__)
/* #undef HAVE_CLOSESOCKET */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef HAVE_CLOSESOCKET */
#elif defined(__ANDROID__)
/* #undef HAVE_CLOSESOCKET */
#else
/* #undef HAVE_CLOSESOCKET */
#endif
/* #undef HAVE_CLOSESOCKET_CAMEL */
#define HAVE_DIRENT_H 1
#define HAVE_OPENDIR 1
#if defined(_WIN32)
/* #undef HAVE_FCNTL */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_FCNTL 1
#elif defined(__APPLE__)
#define HAVE_FCNTL 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_FCNTL 1
#elif defined(__ANDROID__)
#define HAVE_FCNTL 1
#else
#define HAVE_FCNTL 1
#endif
#define HAVE_FCNTL_H 1
#if defined(_WIN32)
/* #undef HAVE_FCNTL_O_NONBLOCK */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_FCNTL_O_NONBLOCK 1
#elif defined(__APPLE__)
#define HAVE_FCNTL_O_NONBLOCK 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_FCNTL_O_NONBLOCK 1
#elif defined(__ANDROID__)
#define HAVE_FCNTL_O_NONBLOCK 1
#else
#define HAVE_FCNTL_O_NONBLOCK 1
#endif
#define HAVE_FREEADDRINFO 1
#define HAVE_FSEEKO 1
#if defined(_WIN32)
/* #undef HAVE_DECL_FSEEKO */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_DECL_FSEEKO 1
#elif defined(__APPLE__)
#define HAVE_DECL_FSEEKO 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_DECL_FSEEKO 1
#elif defined(__ANDROID__)
#define HAVE_DECL_FSEEKO 1
#else
#define HAVE_DECL_FSEEKO 1
#endif
#define HAVE_FTRUNCATE 1
#define HAVE_GETADDRINFO 1
#define HAVE_GETADDRINFO_THREADSAFE 1
#if defined(_WIN32)
/* #undef HAVE_GETEUID */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_GETEUID 1
#elif defined(__APPLE__)
#define HAVE_GETEUID 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_GETEUID 1
#elif defined(__ANDROID__)
#define HAVE_GETEUID 1
#else
#define HAVE_GETEUID 1
#endif
#if defined(_WIN32)
/* #undef HAVE_GETPPID */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_GETPPID 1
#elif defined(__APPLE__)
#define HAVE_GETPPID 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_GETPPID 1
#elif defined(__ANDROID__)
#define HAVE_GETPPID 1
#else
#define HAVE_GETPPID 1
#endif
#if defined(_WIN32)
/* #undef HAVE_GETHOSTBYNAME_R */
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_GETHOSTBYNAME_R */
#elif defined(__APPLE__)
/* #undef HAVE_GETHOSTBYNAME_R */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_GETHOSTBYNAME_R 1
#elif defined(__ANDROID__)
#define HAVE_GETHOSTBYNAME_R 1
#else
#define HAVE_GETHOSTBYNAME_R 1
#endif
/* #undef HAVE_GETHOSTBYNAME_R_3 */
/* #undef HAVE_GETHOSTBYNAME_R_5 */
#if defined(_WIN32)
/* #undef HAVE_GETHOSTBYNAME_R_6 */
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_GETHOSTBYNAME_R_6 */
#elif defined(__APPLE__)
/* #undef HAVE_GETHOSTBYNAME_R_6 */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_GETHOSTBYNAME_R_6 1
#elif defined(__ANDROID__)
#define HAVE_GETHOSTBYNAME_R_6 1
#else
#define HAVE_GETHOSTBYNAME_R_6 1
#endif
#define HAVE_GETHOSTNAME 1
#if defined(_WIN32)
/* #undef HAVE_GETIFADDRS */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_GETIFADDRS 1
#elif defined(__APPLE__)
#define HAVE_GETIFADDRS 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_GETIFADDRS 1
#elif defined(__ANDROID__)
/* #undef HAVE_GETIFADDRS */
#else
#define HAVE_GETIFADDRS 1
#endif
/* #undef HAVE_GETPASS_R */
#define HAVE_GETPEERNAME 1
#define HAVE_GETSOCKNAME 1
#define HAVE_IF_NAMETOINDEX 1
#if defined(_WIN32)
/* #undef HAVE_GETPWUID */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_GETPWUID 1
#elif defined(__APPLE__)
#define HAVE_GETPWUID 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_GETPWUID 1
#elif defined(__ANDROID__)
#define HAVE_GETPWUID 1
#else
#define HAVE_GETPWUID 1
#endif
#if defined(_WIN32)
/* #undef HAVE_GETPWUID_R */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_GETPWUID_R 1
#elif defined(__APPLE__)
#define HAVE_GETPWUID_R 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_GETPWUID_R 1
#elif defined(__ANDROID__)
#define HAVE_GETPWUID_R 1
#else
#define HAVE_GETPWUID_R 1
#endif
#if defined(_WIN32)
/* #undef HAVE_GETRLIMIT */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_GETRLIMIT 1
#elif defined(__APPLE__)
#define HAVE_GETRLIMIT 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_GETRLIMIT 1
#elif defined(__ANDROID__)
#define HAVE_GETRLIMIT 1
#else
#define HAVE_GETRLIMIT 1
#endif
#define HAVE_GETTIMEOFDAY 1
#if defined(_WIN32)
/* #undef HAVE_GLIBC_STRERROR_R */
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_GLIBC_STRERROR_R */
#elif defined(__APPLE__)
/* #undef HAVE_GLIBC_STRERROR_R */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef HAVE_GLIBC_STRERROR_R */
#elif defined(__ANDROID__)
/* #undef HAVE_GLIBC_STRERROR_R */
#else
#define HAVE_GLIBC_STRERROR_R 1
#endif
#if defined(_WIN32)
/* #undef HAVE_GMTIME_R */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_GMTIME_R 1
#elif defined(__APPLE__)
#define HAVE_GMTIME_R 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_GMTIME_R 1
#elif defined(__ANDROID__)
#define HAVE_GMTIME_R 1
#else
#define HAVE_GMTIME_R 1
#endif
/* #undef HAVE_GSSAPI */
/* #undef HAVE_GSSAPI_GSSAPI_GENERIC_H */
/* #undef HAVE_GSSAPI_GSSAPI_H */
/* #undef HAVE_GSSGNU */
#if defined(_WIN32)
/* #undef HAVE_IFADDRS_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_IFADDRS_H 1
#elif defined(__APPLE__)
#define HAVE_IFADDRS_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_IFADDRS_H 1
#elif defined(__ANDROID__)
#define HAVE_IFADDRS_H 1
#else
#define HAVE_IFADDRS_H 1
#endif
#if defined(_WIN32)
/* #undef HAVE_INET_NTOP */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_INET_NTOP 1
#elif defined(__APPLE__)
#define HAVE_INET_NTOP 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_INET_NTOP 1
#elif defined(__ANDROID__)
#define HAVE_INET_NTOP 1
#else
#define HAVE_INET_NTOP 1
#endif
#if defined(_WIN32)
/* #undef HAVE_INET_PTON */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_INET_PTON 1
#elif defined(__APPLE__)
#define HAVE_INET_PTON 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_INET_PTON 1
#elif defined(__ANDROID__)
#define HAVE_INET_PTON 1
#else
#define HAVE_INET_PTON 1
#endif
#if defined(_WIN32)
/* #undef HAVE_SA_FAMILY_T */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_SA_FAMILY_T 1
#elif defined(__APPLE__)
#define HAVE_SA_FAMILY_T 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_SA_FAMILY_T 1
#elif defined(__ANDROID__)
#define HAVE_SA_FAMILY_T 1
#else
#define HAVE_SA_FAMILY_T 1
#endif
#if defined(_WIN32)
#define HAVE_ADDRESS_FAMILY 1
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_ADDRESS_FAMILY */
#elif defined(__APPLE__)
/* #undef HAVE_ADDRESS_FAMILY */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef HAVE_ADDRESS_FAMILY */
#elif defined(__ANDROID__)
/* #undef HAVE_ADDRESS_FAMILY */
#else
/* #undef HAVE_ADDRESS_FAMILY */
#endif
#if defined(_WIN32)
#define HAVE_IOCTLSOCKET 1
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_IOCTLSOCKET */
#elif defined(__APPLE__)
/* #undef HAVE_IOCTLSOCKET */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef HAVE_IOCTLSOCKET */
#elif defined(__ANDROID__)
/* #undef HAVE_IOCTLSOCKET */
#else
/* #undef HAVE_IOCTLSOCKET */
#endif
/* #undef HAVE_IOCTLSOCKET_CAMEL */
/* #undef HAVE_IOCTLSOCKET_CAMEL_FIONBIO */
#if defined(_WIN32)
#define HAVE_IOCTLSOCKET_FIONBIO 1
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_IOCTLSOCKET_FIONBIO */
#elif defined(__APPLE__)
/* #undef HAVE_IOCTLSOCKET_FIONBIO */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef HAVE_IOCTLSOCKET_FIONBIO */
#elif defined(__ANDROID__)
/* #undef HAVE_IOCTLSOCKET_FIONBIO */
#else
/* #undef HAVE_IOCTLSOCKET_FIONBIO */
#endif
#if defined(_WIN32)
/* #undef HAVE_IOCTL_FIONBIO */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_IOCTL_FIONBIO 1
#elif defined(__APPLE__)
#define HAVE_IOCTL_FIONBIO 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_IOCTL_FIONBIO 1
#elif defined(__ANDROID__)
#define HAVE_IOCTL_FIONBIO 1
#else
#define HAVE_IOCTL_FIONBIO 1
#endif
#if defined(_WIN32)
/* #undef HAVE_IOCTL_SIOCGIFADDR */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_IOCTL_SIOCGIFADDR 1
#elif defined(__APPLE__)
#define HAVE_IOCTL_SIOCGIFADDR 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_IOCTL_SIOCGIFADDR 1
#elif defined(__ANDROID__)
#define HAVE_IOCTL_SIOCGIFADDR 1
#else
#define HAVE_IOCTL_SIOCGIFADDR 1
#endif
#if defined(_WIN32)
#define HAVE_IO_H 1
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_IO_H */
#elif defined(__APPLE__)
/* #undef HAVE_IO_H */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef HAVE_IO_H */
#elif defined(__ANDROID__)
/* #undef HAVE_IO_H */
#else
/* #undef HAVE_IO_H */
#endif
/* #undef HAVE_LBER_H */
/* #undef HAVE_LDAP_SSL */
/* #undef HAVE_LDAP_SSL_H */
/* #undef HAVE_LDAP_URL_PARSE */
#define HAVE_LIBGEN_H 1
/* #undef HAVE_LIBIDN2 */
/* #undef HAVE_IDN2_H */
/* #undef HAVE_LIBZ */
/* #undef HAVE_BROTLI */
/* #undef HAVE_ZSTD */
#define HAVE_LOCALE_H 1
#define HAVE_LONGLONG 1
#if defined(_WIN32)
/* #undef HAVE_SUSECONDS_T */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_SUSECONDS_T 1
#elif defined(__APPLE__)
#define HAVE_SUSECONDS_T 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_SUSECONDS_T 1
#elif defined(__ANDROID__)
#define HAVE_SUSECONDS_T 1
#else
#define HAVE_SUSECONDS_T 1
#endif
#if defined(_WIN32)
/* #undef HAVE_MSG_NOSIGNAL */
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_MSG_NOSIGNAL */
#elif defined(__APPLE__)
/* #undef HAVE_MSG_NOSIGNAL */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_MSG_NOSIGNAL 1
#elif defined(__ANDROID__)
#define HAVE_MSG_NOSIGNAL 1
#else
#define HAVE_MSG_NOSIGNAL 1
#endif
#if defined(_WIN32)
/* #undef HAVE_NETDB_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_NETDB_H 1
#elif defined(__APPLE__)
#define HAVE_NETDB_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_NETDB_H 1
#elif defined(__ANDROID__)
#define HAVE_NETDB_H 1
#else
#define HAVE_NETDB_H 1
#endif
#if defined(_WIN32)
/* #undef HAVE_NETINET_IN_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_NETINET_IN_H 1
#elif defined(__APPLE__)
#define HAVE_NETINET_IN_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_NETINET_IN_H 1
#elif defined(__ANDROID__)
#define HAVE_NETINET_IN_H 1
#else
#define HAVE_NETINET_IN_H 1
#endif
#if defined(_WIN32)
/* #undef HAVE_NETINET_IN6_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_NETINET_IN6_H */
#elif defined(__APPLE__)
/* #undef HAVE_NETINET_IN6_H */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_NETINET_IN6_H 1
#elif defined(__ANDROID__)
#define HAVE_NETINET_IN6_H 1
#else
/* #undef HAVE_NETINET_IN6_H */
#endif
#if defined(_WIN32)
/* #undef HAVE_NETINET_TCP_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_NETINET_TCP_H 1
#elif defined(__APPLE__)
#define HAVE_NETINET_TCP_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_NETINET_TCP_H 1
#elif defined(__ANDROID__)
#define HAVE_NETINET_TCP_H 1
#else
#define HAVE_NETINET_TCP_H 1
#endif
#if defined(_WIN32)
/* #undef HAVE_NETINET_UDP_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_NETINET_UDP_H 1
#elif defined(__APPLE__)
#define HAVE_NETINET_UDP_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_NETINET_UDP_H 1
#elif defined(__ANDROID__)
#define HAVE_NETINET_UDP_H 1
#else
#define HAVE_NETINET_UDP_H 1
#endif
#if defined(_WIN32)
/* #undef HAVE_LINUX_TCP_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_LINUX_TCP_H */
#elif defined(__APPLE__)
/* #undef HAVE_LINUX_TCP_H */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_LINUX_TCP_H 1
#elif defined(__ANDROID__)
#define HAVE_LINUX_TCP_H 1
#else
#define HAVE_LINUX_TCP_H 1
#endif
#if defined(_WIN32)
/* #undef HAVE_NET_IF_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_NET_IF_H 1
#elif defined(__APPLE__)
#define HAVE_NET_IF_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_NET_IF_H 1
#elif defined(__ANDROID__)
#define HAVE_NET_IF_H 1
#else
#define HAVE_NET_IF_H 1
#endif
/* #undef HAVE_OLD_GSSMIT */
#if defined(_WIN32)
/* #undef HAVE_PIPE */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_PIPE 1
#elif defined(__APPLE__)
#define HAVE_PIPE 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_PIPE 1
#elif defined(__ANDROID__)
#define HAVE_PIPE 1
#else
#define HAVE_PIPE 1
#endif
#if defined(_WIN32)
/* #undef HAVE_PIPE2 */
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_PIPE2 */
#elif defined(__APPLE__)
/* #undef HAVE_PIPE2 */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_PIPE2 1
#elif defined(__ANDROID__)
#define HAVE_PIPE2 1
#else
#define HAVE_PIPE2 1
#endif
#if defined(_WIN32)
/* #undef HAVE_EVENTFD */
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_EVENTFD */
#elif defined(__APPLE__)
/* #undef HAVE_EVENTFD */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_EVENTFD 1
#elif defined(__ANDROID__)
#define HAVE_EVENTFD 1
#else
#define HAVE_EVENTFD 1
#endif
#if defined(_WIN32)
/* #undef HAVE_POLL */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_POLL 1
#elif defined(__APPLE__)
#define HAVE_POLL 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_POLL 1
#elif defined(__ANDROID__)
#define HAVE_POLL 1
#else
#define HAVE_POLL 1
#endif
#if defined(_WIN32)
/* #undef HAVE_REALPATH */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_REALPATH 1
#elif defined(__APPLE__)
#define HAVE_REALPATH 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_REALPATH 1
#elif defined(__ANDROID__)
#define HAVE_REALPATH 1
#else
#define HAVE_REALPATH 1
#endif
#if defined(_WIN32)
/* #undef HAVE_POLL_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_POLL_H 1
#elif defined(__APPLE__)
#define HAVE_POLL_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_POLL_H 1
#elif defined(__ANDROID__)
#define HAVE_POLL_H 1
#else
#define HAVE_POLL_H 1
#endif
#if defined(_WIN32)
/* #undef HAVE_POSIX_STRERROR_R */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_POSIX_STRERROR_R 1
#elif defined(__APPLE__)
#define HAVE_POSIX_STRERROR_R 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_POSIX_STRERROR_R 1
#elif defined(__ANDROID__)
#define HAVE_POSIX_STRERROR_R 1
#else
/* #undef HAVE_POSIX_STRERROR_R */
#endif
#if defined(_WIN32)
/* #undef HAVE_PTHREAD_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_PTHREAD_H 1
#elif defined(__APPLE__)
#define HAVE_PTHREAD_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_PTHREAD_H 1
#elif defined(__ANDROID__)
#define HAVE_PTHREAD_H 1
#else
#define HAVE_PTHREAD_H 1
#endif
#if defined(_WIN32)
/* #undef HAVE_PWD_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_PWD_H 1
#elif defined(__APPLE__)
#define HAVE_PWD_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_PWD_H 1
#elif defined(__ANDROID__)
#define HAVE_PWD_H 1
#else
#define HAVE_PWD_H 1
#endif
/* #undef HAVE_SSL_SET0_WBIO */
#define HAVE_RECV 1
#define HAVE_SELECT 1
#if defined(_WIN32)
/* #undef HAVE_SCHED_YIELD */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_SCHED_YIELD 1
#elif defined(__APPLE__)
#define HAVE_SCHED_YIELD 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_SCHED_YIELD 1
#elif defined(__ANDROID__)
#define HAVE_SCHED_YIELD 1
#else
#define HAVE_SCHED_YIELD 1
#endif
#define HAVE_SEND 1
#if defined(_WIN32)
/* #undef HAVE_SENDMSG */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_SENDMSG 1
#elif defined(__APPLE__)
#define HAVE_SENDMSG 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_SENDMSG 1
#elif defined(__ANDROID__)
#define HAVE_SENDMSG 1
#else
#define HAVE_SENDMSG 1
#endif
#if defined(_WIN32)
/* #undef HAVE_SENDMMSG */
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_SENDMMSG */
#elif defined(__APPLE__)
/* #undef HAVE_SENDMMSG */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_SENDMMSG 1
#elif defined(__ANDROID__)
#define HAVE_SENDMMSG 1
#else
#define HAVE_SENDMMSG 1
#endif
#define HAVE_STDINT_H 1
#if defined(_WIN32)
/* #undef HAVE_FSETXATTR */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_FSETXATTR 1
#elif defined(__APPLE__)
#define HAVE_FSETXATTR 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_FSETXATTR 1
#elif defined(__ANDROID__)
#define HAVE_FSETXATTR 1
#else
#define HAVE_FSETXATTR 1
#endif
#if defined(_WIN32)
/* #undef HAVE_FSETXATTR_5 */
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_FSETXATTR_5 */
#elif defined(__APPLE__)
/* #undef HAVE_FSETXATTR_5 */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_FSETXATTR_5 1
#elif defined(__ANDROID__)
#define HAVE_FSETXATTR_5 1
#else
#define HAVE_FSETXATTR_5 1
#endif
#if defined(_WIN32)
/* #undef HAVE_FSETXATTR_6 */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_FSETXATTR_6 1
#elif defined(__APPLE__)
#define HAVE_FSETXATTR_6 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef HAVE_FSETXATTR_6 */
#elif defined(__ANDROID__)
/* #undef HAVE_FSETXATTR_6 */
#else
/* #undef HAVE_FSETXATTR_6 */
#endif
#define HAVE_SETLOCALE 1
#if defined(_WIN32)
#define HAVE_SETMODE 1
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_SETMODE */
#elif defined(__APPLE__)
/* #undef HAVE_SETMODE */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef HAVE_SETMODE */
#elif defined(__ANDROID__)
/* #undef HAVE_SETMODE */
#else
/* #undef HAVE_SETMODE */
#endif
#if defined(_WIN32)
#define HAVE__SETMODE 1
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE__SETMODE */
#elif defined(__APPLE__)
/* #undef HAVE__SETMODE */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef HAVE__SETMODE */
#elif defined(__ANDROID__)
/* #undef HAVE__SETMODE */
#else
/* #undef HAVE__SETMODE */
#endif
#if defined(_WIN32)
/* #undef HAVE_SETRLIMIT */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_SETRLIMIT 1
#elif defined(__APPLE__)
#define HAVE_SETRLIMIT 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_SETRLIMIT 1
#elif defined(__ANDROID__)
#define HAVE_SETRLIMIT 1
#else
#define HAVE_SETRLIMIT 1
#endif
/* #undef HAVE_SETSOCKOPT_SO_NONBLOCK */
#if defined(_WIN32)
/* #undef HAVE_SIGACTION */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_SIGACTION 1
#elif defined(__APPLE__)
#define HAVE_SIGACTION 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_SIGACTION 1
#elif defined(__ANDROID__)
#define HAVE_SIGACTION 1
#else
#define HAVE_SIGACTION 1
#endif
#if defined(_WIN32)
/* #undef HAVE_SIGINTERRUPT */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_SIGINTERRUPT 1
#elif defined(__APPLE__)
#define HAVE_SIGINTERRUPT 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_SIGINTERRUPT 1
#elif defined(__ANDROID__)
#define HAVE_SIGINTERRUPT 1
#else
#define HAVE_SIGINTERRUPT 1
#endif
#define HAVE_SIGNAL 1
#if defined(_WIN32)
/* #undef HAVE_SIGSETJMP */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_SIGSETJMP 1
#elif defined(__APPLE__)
#define HAVE_SIGSETJMP 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_SIGSETJMP 1
#elif defined(__ANDROID__)
#define HAVE_SIGSETJMP 1
#else
#define HAVE_SIGSETJMP 1
#endif
#define HAVE_SNPRINTF 1
#define HAVE_SOCKADDR_IN6_SIN6_SCOPE_ID 1
#define HAVE_SOCKET 1
/* #undef HAVE_PROTO_BSDSOCKET_H */
#if defined(_WIN32)
/* #undef HAVE_SOCKETPAIR */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_SOCKETPAIR 1
#elif defined(__APPLE__)
#define HAVE_SOCKETPAIR 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_SOCKETPAIR 1
#elif defined(__ANDROID__)
#define HAVE_SOCKETPAIR 1
#else
#define HAVE_SOCKETPAIR 1
#endif
#define HAVE_STDATOMIC_H 1
#define HAVE_STDBOOL_H 1
#if defined(_WIN32)
/* #undef HAVE_STRCASECMP */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_STRCASECMP 1
#elif defined(__APPLE__)
#define HAVE_STRCASECMP 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_STRCASECMP 1
#elif defined(__ANDROID__)
#define HAVE_STRCASECMP 1
#else
#define HAVE_STRCASECMP 1
#endif
/* #undef HAVE_STRCMPI */
#define HAVE_STRDUP 1
#if defined(_WIN32)
/* #undef HAVE_STRERROR_R */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_STRERROR_R 1
#elif defined(__APPLE__)
#define HAVE_STRERROR_R 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_STRERROR_R 1
#elif defined(__ANDROID__)
#define HAVE_STRERROR_R 1
#else
#define HAVE_STRERROR_R 1
#endif
/* #undef HAVE_STRICMP */
#define HAVE_STRINGS_H 1
/* #undef HAVE_STROPTS_H */
#if defined(_WIN32)
/* #undef HAVE_MEMRCHR */
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_MEMRCHR */
#elif defined(__APPLE__)
/* #undef HAVE_MEMRCHR */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_MEMRCHR 1
#elif defined(__ANDROID__)
#define HAVE_MEMRCHR 1
#else
#define HAVE_MEMRCHR 1
#endif
#define HAVE_STRUCT_SOCKADDR_STORAGE 1
#define HAVE_STRUCT_TIMEVAL 1
#if defined(_WIN32)
/* #undef HAVE_SYS_EVENTFD_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_SYS_EVENTFD_H */
#elif defined(__APPLE__)
/* #undef HAVE_SYS_EVENTFD_H */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_SYS_EVENTFD_H 1
#elif defined(__ANDROID__)
#define HAVE_SYS_EVENTFD_H 1
#else
#define HAVE_SYS_EVENTFD_H 1
#endif
/* #undef HAVE_SYS_FILIO_H */
#if defined(_WIN32)
/* #undef HAVE_SYS_IOCTL_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_SYS_IOCTL_H 1
#elif defined(__APPLE__)
#define HAVE_SYS_IOCTL_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_SYS_IOCTL_H 1
#elif defined(__ANDROID__)
#define HAVE_SYS_IOCTL_H 1
#else
#define HAVE_SYS_IOCTL_H 1
#endif
#define HAVE_SYS_PARAM_H 1
#if defined(_WIN32)
/* #undef HAVE_SYS_POLL_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_SYS_POLL_H 1
#elif defined(__APPLE__)
#define HAVE_SYS_POLL_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_SYS_POLL_H 1
#elif defined(__ANDROID__)
#define HAVE_SYS_POLL_H 1
#else
#define HAVE_SYS_POLL_H 1
#endif
#if defined(_WIN32)
/* #undef HAVE_SYS_RESOURCE_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_SYS_RESOURCE_H 1
#elif defined(__APPLE__)
#define HAVE_SYS_RESOURCE_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_SYS_RESOURCE_H 1
#elif defined(__ANDROID__)
#define HAVE_SYS_RESOURCE_H 1
#else
#define HAVE_SYS_RESOURCE_H 1
#endif
#if defined(_WIN32)
/* #undef HAVE_SYS_SELECT_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_SYS_SELECT_H 1
#elif defined(__APPLE__)
#define HAVE_SYS_SELECT_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_SYS_SELECT_H 1
#elif defined(__ANDROID__)
#define HAVE_SYS_SELECT_H 1
#else
#define HAVE_SYS_SELECT_H 1
#endif
#if defined(_WIN32)
/* #undef HAVE_SYS_SOCKIO_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_SYS_SOCKIO_H 1
#elif defined(__APPLE__)
#define HAVE_SYS_SOCKIO_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef HAVE_SYS_SOCKIO_H */
#elif defined(__ANDROID__)
/* #undef HAVE_SYS_SOCKIO_H */
#else
/* #undef HAVE_SYS_SOCKIO_H */
#endif
#define HAVE_SYS_TYPES_H 1
#if defined(_WIN32)
/* #undef HAVE_SYS_UN_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_SYS_UN_H 1
#elif defined(__APPLE__)
#define HAVE_SYS_UN_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_SYS_UN_H 1
#elif defined(__ANDROID__)
#define HAVE_SYS_UN_H 1
#else
#define HAVE_SYS_UN_H 1
#endif
#if defined(_WIN32)
#define HAVE_SYS_UTIME_H 1
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_SYS_UTIME_H */
#elif defined(__APPLE__)
/* #undef HAVE_SYS_UTIME_H */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef HAVE_SYS_UTIME_H */
#elif defined(__ANDROID__)
/* #undef HAVE_SYS_UTIME_H */
#else
/* #undef HAVE_SYS_UTIME_H */
#endif
#if defined(_WIN32)
/* #undef HAVE_TERMIOS_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_TERMIOS_H 1
#elif defined(__APPLE__)
#define HAVE_TERMIOS_H 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_TERMIOS_H 1
#elif defined(__ANDROID__)
#define HAVE_TERMIOS_H 1
#else
#define HAVE_TERMIOS_H 1
#endif
#if defined(_WIN32)
/* #undef HAVE_TERMIO_H */
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef HAVE_TERMIO_H */
#elif defined(__APPLE__)
/* #undef HAVE_TERMIO_H */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_TERMIO_H 1
#elif defined(__ANDROID__)
#define HAVE_TERMIO_H 1
#else
#define HAVE_TERMIO_H 1
#endif
#define HAVE_UNISTD_H 1
#define HAVE_UTIME 1
#if defined(_WIN32)
/* #undef HAVE_UTIMES */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_UTIMES 1
#elif defined(__APPLE__)
#define HAVE_UTIMES 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define HAVE_UTIMES 1
#elif defined(__ANDROID__)
#define HAVE_UTIMES 1
#else
#define HAVE_UTIMES 1
#endif
#define HAVE_UTIME_H 1
/* #undef HAVE_WRITABLE_ARGV */
/* #undef HAVE_TIME_T_UNSIGNED */
/* #undef NEED_REENTRANT */
#if defined(_WIN32)
#define CURL_OS "Windows"
#elif defined(__APPLE__) && TARGET_OS_OSX
#define CURL_OS "Darwin"
#elif defined(__APPLE__)
#define CURL_OS "Darwin"
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define CURL_OS "Android"
#elif defined(__ANDROID__)
#define CURL_OS "Android"
#else
#define CURL_OS "Linux"
#endif
#define SIZEOF_INT 4
#if defined(_WIN32)
#define SIZEOF_LONG 4
#elif defined(__APPLE__) && TARGET_OS_OSX
#define SIZEOF_LONG 8
#elif defined(__APPLE__)
#define SIZEOF_LONG 8
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define SIZEOF_LONG 8
#elif defined(__ANDROID__)
#define SIZEOF_LONG 8
#else
#define SIZEOF_LONG 8
#endif
#define SIZEOF_LONG_LONG 8
#define SIZEOF_OFF_T 8
#define SIZEOF_CURL_OFF_T 8
#if defined(_WIN32)
#define SIZEOF_CURL_SOCKET_T 8
#elif defined(__APPLE__) && TARGET_OS_OSX
#define SIZEOF_CURL_SOCKET_T 4
#elif defined(__APPLE__)
#define SIZEOF_CURL_SOCKET_T 4
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define SIZEOF_CURL_SOCKET_T 4
#elif defined(__ANDROID__)
#define SIZEOF_CURL_SOCKET_T 4
#else
#define SIZEOF_CURL_SOCKET_T 4
#endif
#define SIZEOF_SIZE_T 8
#define SIZEOF_TIME_T 8
#define STDC_HEADERS 1
/* #undef USE_ARES */
#if defined(_WIN32)
/* #undef USE_THREADS_POSIX */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define USE_THREADS_POSIX 1
#elif defined(__APPLE__)
#define USE_THREADS_POSIX 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define USE_THREADS_POSIX 1
#elif defined(__ANDROID__)
#define USE_THREADS_POSIX 1
#else
#define USE_THREADS_POSIX 1
#endif
#if defined(_WIN32)
#define USE_THREADS_WIN32 1
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef USE_THREADS_WIN32 */
#elif defined(__APPLE__)
/* #undef USE_THREADS_WIN32 */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef USE_THREADS_WIN32 */
#elif defined(__ANDROID__)
/* #undef USE_THREADS_WIN32 */
#else
/* #undef USE_THREADS_WIN32 */
#endif
/* #undef USE_GNUTLS */
/* #undef USE_SSLS_EXPORT */
#if defined(_WIN32)
/* #undef USE_MBEDTLS */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define USE_MBEDTLS 1
#elif defined(__APPLE__)
#define USE_MBEDTLS 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
#define USE_MBEDTLS 1
#elif defined(__ANDROID__)
#define USE_MBEDTLS 1
#else
#define USE_MBEDTLS 1
#endif
/* #undef USE_RUSTLS */
/* #undef USE_WOLFSSL */
/* #undef HAVE_WOLFSSL_GET_PEER_CERTIFICATE */
/* #undef HAVE_WOLFSSL_USEALPN */
/* #undef HAVE_WOLFSSL_DES_ECB_ENCRYPT */
/* #undef HAVE_WOLFSSL_BIO_NEW */
/* #undef HAVE_WOLFSSL_BIO_SET_SHUTDOWN */
/* #undef USE_LIBSSH */
/* #undef USE_LIBSSH2 */
/* #undef USE_WOLFSSH */
/* #undef USE_LIBPSL */
/* #undef USE_OPENLDAP */
/* #undef USE_OPENSSL */
/* #undef USE_AMISSL */
/* #undef USE_LIBRTMP */
/* #undef USE_GSASL */
/* #undef USE_LIBUV */
/* #undef HAVE_UV_H */
/* #undef CURL_DISABLE_OPENSSL_AUTO_LOAD_CONFIG */
/* #undef USE_NGHTTP2 */
/* #undef USE_NGTCP2 */
/* #undef USE_NGHTTP3 */
/* #undef USE_QUICHE */
/* #undef USE_OPENSSL_QUIC */
/* #undef OPENSSL_QUIC_API2 */
/* #undef HAVE_QUICHE_CONN_SET_QLOG_FD */
/* #undef USE_MSH3 */
#define USE_UNIX_SOCKETS 1
#if defined(_WIN32)
#define USE_WIN32_LARGE_FILES 1
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef USE_WIN32_LARGE_FILES */
#elif defined(__APPLE__)
/* #undef USE_WIN32_LARGE_FILES */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef USE_WIN32_LARGE_FILES */
#elif defined(__ANDROID__)
/* #undef USE_WIN32_LARGE_FILES */
#else
/* #undef USE_WIN32_LARGE_FILES */
#endif
#if defined(_WIN32)
#define USE_WINDOWS_SSPI 1
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef USE_WINDOWS_SSPI */
#elif defined(__APPLE__)
/* #undef USE_WINDOWS_SSPI */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef USE_WINDOWS_SSPI */
#elif defined(__ANDROID__)
/* #undef USE_WINDOWS_SSPI */
#else
/* #undef USE_WINDOWS_SSPI */
#endif
#if defined(_WIN32)
#define USE_SCHANNEL 1
#elif defined(__APPLE__) && TARGET_OS_OSX
/* #undef USE_SCHANNEL */
#elif defined(__APPLE__)
/* #undef USE_SCHANNEL */
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef USE_SCHANNEL */
#elif defined(__ANDROID__)
/* #undef USE_SCHANNEL */
#else
/* #undef USE_SCHANNEL */
#endif
/* #undef USE_WATT32 */
/* #undef CURL_WITH_MULTI_SSL */
#define _FILE_OFFSET_BITS 64
/* #undef ssize_t */
#if defined(_WIN32)
/* #undef HAVE_MACH_ABSOLUTE_TIME */
#elif defined(__APPLE__) && TARGET_OS_OSX
#define HAVE_MACH_ABSOLUTE_TIME 1
#elif defined(__APPLE__)
#define HAVE_MACH_ABSOLUTE_TIME 1
#elif defined(__ANDROID__) && __ANDROID_API__ >= 24
/* #undef HAVE_MACH_ABSOLUTE_TIME */
#elif defined(__ANDROID__)
/* #undef HAVE_MACH_ABSOLUTE_TIME */
#else
/* #undef HAVE_MACH_ABSOLUTE_TIME */
#endif
/* #undef USE_WIN32_IDN */
/* #undef USE_APPLE_IDN */
/* #undef HAVE_OPENSSL_SRP */
/* #undef HAVE_GNUTLS_SRP */
/* #undef USE_TLS_SRP */
/* #undef USE_HTTPSRR */
/* #undef USE_ECH */
/* #undef HAVE_WOLFSSL_CTX_GENERATEECHCONFIG */
/* #undef HAVE_SSL_SET1_ECH_CONFIG_LIST */

#endif
