/*
===========================================================================
UDP transport for the Jedi Outcast singleplayer engine.

The singleplayer engine retains the whole Quake 3 protocol layer -- netchan
sequencing, delta-compressed snapshots, usercmd serialisation -- but Raven
removed the transport beneath it. NET_Init was an empty inline stub,
NET_SendPacket silently discarded anything that was not NA_LOOPBACK, and
NET_StringToAdr resolved only the literal string "localhost".

This file restores a UDP socket layer so a second client can connect over
the network. It is a reduced port of codemp/qcommon/net_ip.cpp: SOCKS
proxying, broadcast scanning, and LAN-address heuristics are omitted, as
nothing in the singleplayer engine uses them.

Networking is off unless net_enabled is set, so a plain singleplayer session
opens no socket and behaves exactly as before. When enabled, the socket binds
every interface by default (net_ip "0.0.0.0") so a LAN client can reach it.
===========================================================================
*/

#include "q_shared.h"
#include "qcommon.h"

// Cross-platform socket layer. The POSIX and winsock APIs differ only in a
// handful of spellings; unify them the same way codemp/qcommon/net_ip.cpp does
// so the identical socket code below compiles on both. Only the syscall names
// change — the behaviour is unchanged.
#ifdef _WIN32
	#include <winsock.h>

	typedef int socklen_t;

	#undef EAGAIN
	#undef EADDRNOTAVAIL
	#undef ECONNRESET

	#define EAGAIN WSAEWOULDBLOCK
	#define EADDRNOTAVAIL WSAEADDRNOTAVAIL
	#define ECONNRESET WSAECONNRESET

	#define socketError WSAGetLastError( )

	static WSADATA winsockdata;
	static qboolean winsockInitialized = qfalse;
#else
	#include <arpa/inet.h>
	#include <errno.h>
	#include <netdb.h>
	#include <netinet/in.h>
	#include <sys/ioctl.h>
	#include <sys/socket.h>
	#include <sys/types.h>
	#include <sys/select.h>
	#include <unistd.h>

	typedef int SOCKET;
	#define INVALID_SOCKET (-1)
	#define SOCKET_ERROR   (-1)
	#define closesocket    close
	#define ioctlsocket    ioctl
	#define socketError    errno
#endif

static int   ip_socket = INVALID_SOCKET;

static cvar_t *net_enabled;
static cvar_t *net_ip;
static cvar_t *net_port;

/*
====================
NET_ErrorString
====================
*/
const char *NET_ErrorString( void ) {
#ifdef _WIN32
	switch ( socketError ) {
	case WSAEINTR:           return "WSAEINTR";
	case WSAEBADF:           return "WSAEBADF";
	case WSAEACCES:          return "WSAEACCES";
	case WSAEFAULT:          return "WSAEFAULT";
	case WSAEINVAL:          return "WSAEINVAL";
	case WSAEMFILE:          return "WSAEMFILE";
	case WSAEWOULDBLOCK:     return "WSAEWOULDBLOCK";
	case WSAEINPROGRESS:     return "WSAEINPROGRESS";
	case WSAEALREADY:        return "WSAEALREADY";
	case WSAENOTSOCK:        return "WSAENOTSOCK";
	case WSAEDESTADDRREQ:    return "WSAEDESTADDRREQ";
	case WSAEMSGSIZE:        return "WSAEMSGSIZE";
	case WSAEPROTOTYPE:      return "WSAEPROTOTYPE";
	case WSAENOPROTOOPT:     return "WSAENOPROTOOPT";
	case WSAEPROTONOSUPPORT: return "WSAEPROTONOSUPPORT";
	case WSAESOCKTNOSUPPORT: return "WSAESOCKTNOSUPPORT";
	case WSAEOPNOTSUPP:      return "WSAEOPNOTSUPP";
	case WSAEPFNOSUPPORT:    return "WSAEPFNOSUPPORT";
	case WSAEAFNOSUPPORT:    return "WSAEAFNOSUPPORT";
	case WSAEADDRINUSE:      return "WSAEADDRINUSE";
	case WSAEADDRNOTAVAIL:   return "WSAEADDRNOTAVAIL";
	case WSAENETDOWN:        return "WSAENETDOWN";
	case WSAENETUNREACH:     return "WSAENETUNREACH";
	case WSAENETRESET:       return "WSAENETRESET";
	case WSAECONNABORTED:    return "WSAECONNABORTED";
	case WSAECONNRESET:      return "WSAECONNRESET";
	case WSAENOBUFS:         return "WSAENOBUFS";
	case WSAEISCONN:         return "WSAEISCONN";
	case WSAENOTCONN:        return "WSAENOTCONN";
	case WSAESHUTDOWN:       return "WSAESHUTDOWN";
	case WSAETIMEDOUT:       return "WSAETIMEDOUT";
	case WSAECONNREFUSED:    return "WSAECONNREFUSED";
	case WSAEHOSTDOWN:       return "WSAEHOSTDOWN";
	case WSAEHOSTUNREACH:    return "WSAEHOSTUNREACH";
	case WSASYSNOTREADY:     return "WSASYSNOTREADY";
	case WSAVERNOTSUPPORTED: return "WSAVERNOTSUPPORTED";
	case WSANOTINITIALISED:  return "WSANOTINITIALISED";
	default:                 return "NO ERROR";
	}
#else
	return strerror( socketError );
#endif
}

/*
====================
NetadrToSockadr
====================
*/
static void NetadrToSockadr( const netadr_t *a, struct sockaddr_in *s ) {
	memset( s, 0, sizeof(*s) );

	if ( a->type == NA_BROADCAST ) {
		s->sin_family = AF_INET;
		s->sin_port = a->port;
		s->sin_addr.s_addr = INADDR_BROADCAST;
	} else if ( a->type == NA_IP ) {
		s->sin_family = AF_INET;
		memcpy( &s->sin_addr, a->ip, sizeof(s->sin_addr) );
		s->sin_port = a->port;
	}
}

/*
====================
SockadrToNetadr
====================
*/
static void SockadrToNetadr( struct sockaddr_in *s, netadr_t *a ) {
	a->type = NA_IP;
	memcpy( a->ip, &s->sin_addr, sizeof(a->ip) );
	a->port = s->sin_port;
}

/*
====================
Sys_StringToSockaddr
====================
*/
static qboolean Sys_StringToSockaddr( const char *s, struct sockaddr_in *sadr ) {
	struct hostent *h;

	memset( sadr, 0, sizeof(*sadr) );
	sadr->sin_family = AF_INET;
	sadr->sin_port = 0;

	if ( s[0] >= '0' && s[0] <= '9' ) {
		sadr->sin_addr.s_addr = inet_addr( s );
		if ( sadr->sin_addr.s_addr == INADDR_NONE ) {
			return qfalse;
		}
	} else {
		if ( ( h = gethostbyname( s ) ) == NULL ) {
			return qfalse;
		}
		sadr->sin_addr.s_addr = *(uint32_t *)h->h_addr_list[0];
	}

	return qtrue;
}

/*
====================
Sys_StringToAdr

Accepts "host" or "host:port".
====================
*/
qboolean Sys_StringToAdr( const char *s, netadr_t *a ) {
	struct sockaddr_in sadr;
	char	base[MAX_STRING_CHARS];
	char	*port = NULL;

	Q_strncpyz( base, s, sizeof(base) );

	// split a trailing :port, if present
	for ( char *p = base; *p; p++ ) {
		if ( *p == ':' ) {
			*p = '\0';
			port = p + 1;
			break;
		}
	}

	if ( !Sys_StringToSockaddr( base, &sadr ) ) {
		return qfalse;
	}

	SockadrToNetadr( &sadr, a );

	if ( port ) {
		a->port = htons( (short)atoi( port ) );
	}

	return qtrue;
}

/*
====================
NET_IPSocket

A non-blocking UDP socket bound to the given interface and port.
====================
*/
static int NET_IPSocket( const char *net_interface, int port ) {
	struct sockaddr_in	address;
	int					newsocket;
	int					i = 1;

	// "localhost", "0.0.0.0" and the empty string all map to INADDR_ANY below.
	// Say so, rather than echoing an interface name we do not bind to.
	if ( net_interface && net_interface[0]
		&& Q_stricmp( net_interface, "localhost" )
		&& Q_stricmp( net_interface, "0.0.0.0" ) ) {
		Com_Printf( "Opening IP socket: %s:%i\n", net_interface, port );
	} else {
		Com_Printf( "Opening IP socket: 0.0.0.0:%i (all interfaces)\n", port );
	}

	if ( ( newsocket = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP ) ) == INVALID_SOCKET ) {
		Com_Printf( "WARNING: NET_IPSocket: socket: %s\n", NET_ErrorString() );
		return INVALID_SOCKET;
	}

	// make it non-blocking; the engine polls rather than waits
	u_long _true = 1;
	if ( ioctlsocket( newsocket, FIONBIO, &_true ) == SOCKET_ERROR ) {
		Com_Printf( "WARNING: NET_IPSocket: FIONBIO: %s\n", NET_ErrorString() );
		closesocket( newsocket );
		return INVALID_SOCKET;
	}

	if ( setsockopt( newsocket, SOL_SOCKET, SO_BROADCAST, (char *)&i, sizeof(i) ) == SOCKET_ERROR ) {
		Com_Printf( "WARNING: NET_IPSocket: SO_BROADCAST: %s\n", NET_ErrorString() );
	}

	memset( &address, 0, sizeof(address) );
	address.sin_family = AF_INET;

	if ( !net_interface || !net_interface[0]
		|| !Q_stricmp( net_interface, "localhost" )
		|| !Q_stricmp( net_interface, "0.0.0.0" ) ) {
		address.sin_addr.s_addr = INADDR_ANY;
	} else if ( !Sys_StringToSockaddr( net_interface, &address ) ) {
		closesocket( newsocket );
		return INVALID_SOCKET;
	}

	address.sin_port = htons( (short)port );

	if ( bind( newsocket, (struct sockaddr *)&address, sizeof(address) ) == SOCKET_ERROR ) {
		Com_Printf( "WARNING: NET_IPSocket: bind: %s\n", NET_ErrorString() );
		closesocket( newsocket );
		return INVALID_SOCKET;
	}

	return newsocket;
}

/*
====================
NET_GetPacket

Reads one waiting datagram. Returns qfalse when the socket is closed or
would block.
====================
*/
qboolean NET_GetPacket( netadr_t *net_from, msg_t *net_message ) {
	struct sockaddr_in	from;
	socklen_t			fromlen;
	int					ret;

	if ( ip_socket == INVALID_SOCKET ) {
		return qfalse;
	}

	fromlen = sizeof( from );
	ret = recvfrom( ip_socket, (char *)net_message->data, net_message->maxsize, 0,
					(struct sockaddr *)&from, &fromlen );


	if ( ret == SOCKET_ERROR ) {
		// EAGAIN is aliased to WSAEWOULDBLOCK on Windows (see the header block);
		// EWOULDBLOCK is not portably defined there, so key off EAGAIN only.
		int err = socketError;
		if ( err == EAGAIN || err == ECONNRESET ) {
			return qfalse;
		}
		Com_Printf( "NET_GetPacket: %s\n", NET_ErrorString() );
		return qfalse;
	}

	SockadrToNetadr( &from, net_from );
	net_message->readcount = 0;

	if ( ret >= net_message->maxsize ) {
		Com_Printf( "Oversize packet from %s\n", NET_AdrToString( *net_from ) );
		return qfalse;
	}

	net_message->cursize = ret;
	return qtrue;
}

/*
====================
Sys_SendPacket
====================
*/
void Sys_SendPacket( int length, const void *data, const netadr_t *to ) {
	struct sockaddr_in	addr;
	int					ret;

	if ( to->type != NA_BROADCAST && to->type != NA_IP ) {
		Com_Error( ERR_FATAL, "Sys_SendPacket: bad address type" );
		return;
	}

	if ( ip_socket == INVALID_SOCKET ) {
		return;
	}

	NetadrToSockadr( to, &addr );

	ret = sendto( ip_socket, (const char *)data, length, 0,
				  (struct sockaddr *)&addr, sizeof(addr) );

	if ( ret == SOCKET_ERROR ) {
		int err = socketError;
		// a full send buffer is not an error worth reporting (EAGAIN is aliased
		// to WSAEWOULDBLOCK on Windows)
		if ( err == EAGAIN ) {
			return;
		}
		// some links refuse broadcasts
		if ( err == EADDRNOTAVAIL && to->type == NA_BROADCAST ) {
			return;
		}
		Com_Printf( "NET_SendPacket: %s\n", NET_ErrorString() );
	}
}

/*
====================
NET_IsSocketOpen
====================
*/
qboolean NET_IsSocketOpen( void ) {
	return (qboolean)( ip_socket != INVALID_SOCKET );
}

/*
====================
NET_OpenIP
====================
*/
static void NET_OpenIP( void ) {
	int port = net_port->integer;

	if ( ip_socket != INVALID_SOCKET ) {
		return;
	}

#ifdef _WIN32
	// Winsock must be initialised before any socket call, once per process.
	if ( !winsockInitialized ) {
		if ( WSAStartup( MAKEWORD( 1, 1 ), &winsockdata ) != 0 ) {
			Com_Printf( "WARNING: Winsock initialization failed: %s\n", NET_ErrorString() );
			return;
		}
		winsockInitialized = qtrue;
		Com_DPrintf( "Winsock initialized\n" );
	}
#endif

	// try a small range, as the requested port may be taken
	for ( int i = 0; i < 10; i++ ) {
		ip_socket = NET_IPSocket( net_ip->string, port + i );
		if ( ip_socket != INVALID_SOCKET ) {
			Cvar_SetValue( "net_port", port + i );
			return;
		}
	}

	Com_Printf( "WARNING: Couldn't bind to a v4 ip address.\n" );
}

/*
====================
NET_Shutdown
====================
*/
void NET_Shutdown( void ) {
	if ( ip_socket != INVALID_SOCKET ) {
		closesocket( ip_socket );
		ip_socket = INVALID_SOCKET;
	}
#ifdef _WIN32
	if ( winsockInitialized ) {
		WSACleanup();
		winsockInitialized = qfalse;
	}
#endif
}

/*
====================
NET_Init

Networking is off unless net_enabled is set, so a plain singleplayer
session never opens a socket and behaves exactly as it did before.
====================
*/
void NET_Init( void ) {
	net_enabled = Cvar_Get( "net_enabled", "0", CVAR_LATCH | CVAR_ARCHIVE );
	// Bind every interface by default so a LAN client can reach the server.
	// Set net_ip to a specific address to restrict it, or to "localhost" for
	// loopback only.
	net_ip      = Cvar_Get( "net_ip",      "0.0.0.0", CVAR_LATCH );
	net_port    = Cvar_Get( "net_port",    "29070", CVAR_LATCH );

	if ( !net_enabled->integer ) {
		return;
	}

	NET_OpenIP();
}

/*
====================
NET_Restart

Re-open the UDP socket to reflect the current net_enabled / net_ip / net_port
cvar values, at runtime. net_enabled and net_port are CVAR_LATCH and are only
read in NET_Init at startup; re-fetching them through Cvar_Get here applies any
latched value (see Cvar_Get's latch handling), so a game that started as a plain
loopback session can begin hosting without a restart. Used by the coop_host
command. NET_Init's own semantics are unchanged.
====================
*/
void NET_Restart( void ) {
	NET_Shutdown();

	// Re-fetch the cvars so a value that was latched (set after startup) takes
	// effect now.
	net_enabled = Cvar_Get( "net_enabled", "0", CVAR_LATCH | CVAR_ARCHIVE );
	net_ip      = Cvar_Get( "net_ip",      "0.0.0.0", CVAR_LATCH );
	net_port    = Cvar_Get( "net_port",    "29070", CVAR_LATCH );

	if ( !net_enabled->integer ) {
		return;
	}

	NET_OpenIP();
}
