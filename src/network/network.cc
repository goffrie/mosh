/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

#include "config.h"

#include <sys/un.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>

#include "dos_assert.h"
#include "byteorder.h"
#include "network.h"
#include "crypto.h"

#include "timestamp.h"

using namespace std;
using namespace Network;
using namespace Crypto;

const uint64_t DIRECTION_MASK = uint64_t(1) << 63;
const uint64_t SEQUENCE_MASK = uint64_t(-1) ^ DIRECTION_MASK;

int
addreq (const sockaddr_storage *a, const sockaddr_storage *b)
{
  if (a->ss_family != b->ss_family)
    return 0;
  switch (a->ss_family) {
  case AF_INET:
    {
      const struct sockaddr_in *aa = (const struct sockaddr_in *) a;
      const struct sockaddr_in *bb = (const struct sockaddr_in *) b;
      return (aa->sin_addr.s_addr == bb->sin_addr.s_addr
              && aa->sin_port == bb->sin_port);
    }
  case AF_INET6:
    {
      const struct sockaddr_in6 *aa = (const struct sockaddr_in6 *) a;
      const struct sockaddr_in6 *bb = (const struct sockaddr_in6 *) b;
      return (!memcmp (&aa->sin6_addr, &bb->sin6_addr, sizeof (aa->sin6_addr))
              && aa->sin6_port == bb->sin6_port);
    }
  case AF_UNIX:
    {
      const struct sockaddr_un *aa = (const struct sockaddr_un *) a;
      const struct sockaddr_un *bb = (const struct sockaddr_un *) b;
      return !strcmp (aa->sun_path, bb->sun_path);
    }
  }
  fprintf (stderr, "addrhash: unknown address family %d\n",
           a->ss_family);
  abort ();
}

/* Read in packet from coded string */
Packet::Packet( string coded_packet, Session *session )
  : seq( -1 ),
    direction( TO_SERVER ),
    timestamp( -1 ),
    timestamp_reply( -1 ),
    payload()
{
  Message message = session->decrypt( coded_packet );

  direction = (message.nonce.val() & DIRECTION_MASK) ? TO_CLIENT : TO_SERVER;
  seq = message.nonce.val() & SEQUENCE_MASK;

  dos_assert( message.text.size() >= 2 * sizeof( uint16_t ) );

  uint16_t *data = (uint16_t *)message.text.data();
  timestamp = be16toh( data[ 0 ] );
  timestamp_reply = be16toh( data[ 1 ] );

  payload = string( message.text.begin() + 2 * sizeof( uint16_t ), message.text.end() );
}

/* Output coded string from packet */
string Packet::tostring( Session *session )
{
  uint64_t direction_seq = (uint64_t( direction == TO_CLIENT ) << 63) | (seq & SEQUENCE_MASK);

  uint16_t ts_net[ 2 ] = { static_cast<uint16_t>( htobe16( timestamp ) ),
                           static_cast<uint16_t>( htobe16( timestamp_reply ) ) };

  string timestamps = string( (char *)ts_net, 2 * sizeof( uint16_t ) );

  return session->encrypt( Message( Nonce( direction_seq ), timestamps + payload ) );
}

Packet Connection::new_packet( string &s_payload )
{
  uint16_t outgoing_timestamp_reply = -1;

  uint64_t now = timestamp();

  if ( now - saved_timestamp_received_at < 1000 ) { /* we have a recent received timestamp */
    /* send "corrected" timestamp advanced by how long we held it */
    outgoing_timestamp_reply = saved_timestamp + (now - saved_timestamp_received_at);
    saved_timestamp = -1;
    saved_timestamp_received_at = 0;
  }

  Packet p( next_seq++, direction, timestamp16(), outgoing_timestamp_reply, s_payload );

  return p;
}

void Connection::hop_port( void )
{
  assert( !server );

  if ( close( sock ) < 0 ) {
    throw NetworkException( "close", errno );
  }

  assert(!"Not implemented");
}

void Connection::setup( int family, int socktype, int protocol )
{
  if ( sock >= 0 )
    close(sock);

  /* create socket */
  sock = socket( family, socktype, protocol );
  // TODO: nein
  if ( sock < 0 ) {
    throw NetworkException( "socket", errno );
  }

  last_port_choice = timestamp();

  /* Disable path MTU discovery */
#ifdef HAVE_IP_MTU_DISCOVER
  char flag = IP_PMTUDISC_DONT;
  socklen_t optlen = sizeof( flag );
  if ( setsockopt( sock, IPPROTO_IP, IP_MTU_DISCOVER, &flag, optlen ) < 0 ) {
    throw NetworkException( "setsockopt", errno );
  }
#endif

  /* set diffserv values to AF42 + ECT */
  uint8_t dscp = 0x92;
  if ( setsockopt( sock, IPPROTO_IP, IP_TOS, &dscp, 1) < 0 ) {
    //    perror( "setsockopt( IP_TOS )" );
  }

  /* request explicit congestion notification on received datagrams */
#ifdef HAVE_IP_RECVTOS
  char tosflag = true;
  socklen_t tosoptlen = sizeof( tosflag );
  if ( setsockopt( sock, IPPROTO_IP, IP_RECVTOS, &tosflag, tosoptlen ) < 0 ) {
    perror( "setsockopt( IP_RECVTOS )" );
  }
#endif
}

Connection::Connection( const char *desired_ip, const char *desired_port ) /* server */
  : sock( -1 ),
    has_remote_addr( false ),
    remote_addr(),
    remote_addr_len( 0 ),
    server( true ),
    MTU( SEND_MTU ),
    key(),
    session( key ),
    direction( TO_CLIENT ),
    next_seq( 0 ),
    saved_timestamp( -1 ),
    saved_timestamp_received_at( 0 ),
    expected_receiver_seq( 0 ),
    last_heard( -1 ),
    last_port_choice( -1 ),
    last_roundtrip_success( -1 ),
    RTT_hit( false ),
    SRTT( 1000 ),
    RTTVAR( 500 ),
    have_send_exception( false ),
    send_exception()
{
  /* The mosh wrapper always gives an IP request, in order
     to deal with multihomed servers. The port is optional. */

  /* If an IP request is given, we try to bind to that IP, but we also
     try INADDR_ANY. If a port request is given, we bind only to that port. */

  /* convert port number */
  long int desired_port_no = 0;

  if ( desired_port ) {
    char *end;
    errno = 0;
    desired_port_no = strtol( desired_port, &end, 10 );
    if ( (errno != 0) || (end != desired_port + strlen( desired_port )) ) {
      throw NetworkException( "Invalid port number", errno );
    }
  }

  if ( (desired_port_no < 0) || (desired_port_no > 65535) ) {
    throw NetworkException( "Port number outside valid range [0..65535]", 0 );
  }

  /* try to bind to desired IP first, if any */
  if ( desired_ip ) {
    try {
      if ( try_bind( desired_ip, desired_port_no ) ) { return; }
    } catch ( const NetworkException& e ) {
      /* TODO 
      struct in_addr sin_addr;
      sin_addr.s_addr = desired_ip_addr;
      fprintf( stderr, "Error binding to IP %s: %s: %s\n",
	       inet_ntoa( sin_addr ),
	       e.function.c_str(), strerror( e.the_errno ) );
               */
    }
  }

  /* now try any local interface */
  try {
    if ( try_bind( NULL, desired_port_no ) ) { return; }
  } catch ( const NetworkException& e ) {
    fprintf( stderr, "Error binding to any interface: %s: %s\n",
	     e.function.c_str(), strerror( e.the_errno ) );
    throw; /* this time it's fatal */
  }

  assert( false );
  throw NetworkException( "Could not bind", errno );
}

bool Connection::try_bind( const char *node, int port )
{
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  char portstr[6];
  int ret;

  memset(&hints, '\0', sizeof(struct addrinfo));
  /* Use UDP */
  hints.ai_socktype = SOCK_DGRAM;
  /* Use either IPv4 or IPv6, whichever is preferred */
  hints.ai_family = AF_UNSPEC;
  /* Request a listening socket (node == NULL will mean INADDR_ANY in the
   * correct protocol family) */
  hints.ai_flags = AI_PASSIVE;

  int search_low = PORT_RANGE_LOW, search_high = PORT_RANGE_HIGH;

  if ( port != 0 ) { /* port preference */
    search_low = search_high = port;
  }

  for ( int i = search_low; i <= search_high; i++ ) {
    snprintf(portstr, sizeof(portstr), "%u", i);

    ret = getaddrinfo(node, portstr, &hints, &result);
    if ( ret != 0 ) {
      fprintf(stderr, "Failed resolving %s:%u: %s\n",
              node, port, gai_strerror(ret));
      // XXX: Does this exception type assume that the second argument is an
      // errno? Because in our case, we need to call gai_strerror
      throw NetworkException( "Could not resolve", ret );
    }

    for ( rp = result; rp != NULL; rp = rp->ai_next ) {
      setup( rp->ai_family, rp->ai_socktype, rp->ai_protocol );

      if ( bind( sock, rp->ai_addr, rp->ai_addrlen ) == 0 )
        break; /* Success */
      int bind_errno = errno;

      char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
      if ( getnameinfo( rp->ai_addr, rp->ai_addrlen, hbuf, sizeof(hbuf),
                        sbuf, sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV ) == 0 )
        fprintf(stderr, "Warning: Could not bind to %s:%s: %s\n", hbuf, sbuf, strerror(bind_errno));
    }

    if ( rp == NULL && i == search_high ) { /* last port to search */
      throw NetworkException( "bind", errno );
    }

    if ( rp ) {
      return true;
    }
  }

  assert( false );
  return false;
}

Connection::Connection( const char *key_str, const char *ip, int port ) /* client */
  : sock( -1 ),
    has_remote_addr( false ),
    remote_addr(),
    remote_addr_len( 0 ),
    server( false ),
    MTU( SEND_MTU ),
    key( key_str ),
    session( key ),
    direction( TO_SERVER ),
    next_seq( 0 ),
    saved_timestamp( -1 ),
    saved_timestamp_received_at( 0 ),
    expected_receiver_seq( 0 ),
    last_heard( -1 ),
    last_port_choice( -1 ),
    last_roundtrip_success( -1 ),
    RTT_hit( false ),
    SRTT( 1000 ),
    RTTVAR( 500 ),
    have_send_exception( false ),
    send_exception()
{
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  char portstr[6];
  int ret;

  memset(&hints, '\0', sizeof(struct addrinfo));
  /* Use UDP */
  hints.ai_socktype = SOCK_DGRAM;
  /* Use either IPv4 or IPv6, whichever is preferred */
  hints.ai_family = AF_UNSPEC;

  snprintf(portstr, sizeof(portstr), "%u", port);

  ret = getaddrinfo(ip, portstr, &hints, &result);
  if ( ret != 0 ) {
    fprintf(stderr, "Failed resolving %s:%u: %s\n",
            ip, port, gai_strerror(ret));
    // XXX: Does this exception type assume that the second argument is an
    // errno? Because in our case, we need to call gai_strerror
    throw NetworkException( "Could not resolve", ret );
  }

  for ( rp = result; rp != NULL; rp = rp->ai_next ) {
    setup( rp->ai_family, rp->ai_socktype, rp->ai_protocol );

    memcpy( &remote_addr, rp->ai_addr, rp->ai_addrlen );
    remote_addr_len = rp->ai_addrlen;
    break;
  }

  if ( rp == NULL ) {
    throw NetworkException( "bind", errno );
  }

  has_remote_addr = true;
}

void Connection::send( string s )
{
  if ( !has_remote_addr ) {
    return;
  }

  Packet px = new_packet( s );

  string p = px.tostring( &session );

  ssize_t bytes_sent = sendto( sock, p.data(), p.size(), 0,
			       (sockaddr *)&remote_addr, remote_addr_len );

  if ( bytes_sent == static_cast<ssize_t>( p.size() ) ) {
    have_send_exception = false;
  } else {
    /* Notify the frontend on sendto() failure, but don't alter control flow.
       sendto() success is not very meaningful because packets can be lost in
       flight anyway. */
    have_send_exception = true;
    send_exception = NetworkException( "sendto", errno );
  }

  uint64_t now = timestamp();
  if ( server ) {
    if ( now - last_heard > SERVER_ASSOCIATION_TIMEOUT ) {
      has_remote_addr = false;
      fprintf( stderr, "Server now detached from client.\n" );
    }
  } else { /* client */
    if ( ( now - last_port_choice > PORT_HOP_INTERVAL )
	 && ( now - last_roundtrip_success > PORT_HOP_INTERVAL ) ) {
      hop_port();
    }
  }
}

string Connection::recv( void )
{
  /* receive source address, ECN, and payload in msghdr structure */
  struct sockaddr_storage packet_remote_addr;
  struct msghdr header;
  struct iovec msg_iovec;

  char msg_payload[ Session::RECEIVE_MTU ];
  char msg_control[ Session::RECEIVE_MTU ];

  /* receive source address */
  header.msg_name = &packet_remote_addr;
  header.msg_namelen = sizeof( packet_remote_addr );

  /* receive payload */
  msg_iovec.iov_base = msg_payload;
  msg_iovec.iov_len = Session::RECEIVE_MTU;
  header.msg_iov = &msg_iovec;
  header.msg_iovlen = 1;

  /* receive explicit congestion notification */
  header.msg_control = msg_control;
  header.msg_controllen = Session::RECEIVE_MTU;

  /* receive flags */
  header.msg_flags = 0;

  ssize_t received_len = recvmsg( sock, &header, 0 );

  if ( received_len < 0 ) {
    throw NetworkException( "recvfrom", errno );
  }

  if ( header.msg_flags & MSG_TRUNC ) {
    throw NetworkException( "Received oversize datagram", errno );
  }

  /* receive ECN */
  bool congestion_experienced = false;

  struct cmsghdr *ecn_hdr = CMSG_FIRSTHDR( &header );
  if ( ecn_hdr
       && (ecn_hdr->cmsg_level == IPPROTO_IP)
       && (ecn_hdr->cmsg_type == IP_TOS) ) {
    /* got one */
    uint8_t *ecn_octet_p = (uint8_t *)CMSG_DATA( ecn_hdr );
    assert( ecn_octet_p );

    if ( (*ecn_octet_p & 0x03) == 0x03 ) {
      congestion_experienced = true;
    }
  }

  Packet p( string( msg_payload, received_len ), &session );

  dos_assert( p.direction == (server ? TO_SERVER : TO_CLIENT) ); /* prevent malicious playback to sender */

  if ( p.seq >= expected_receiver_seq ) { /* don't use out-of-order packets for timestamp or targeting */
    expected_receiver_seq = p.seq + 1; /* this is security-sensitive because a replay attack could otherwise
					  screw up the timestamp and targeting */

    if ( p.timestamp != uint16_t(-1) ) {
      saved_timestamp = p.timestamp;
      saved_timestamp_received_at = timestamp();

      if ( congestion_experienced ) {
	/* signal counterparty to slow down */
	/* this will gradually slow the counterparty down to the minimum frame rate */
	saved_timestamp -= CONGESTION_TIMESTAMP_PENALTY;
	if ( server ) {
	  fprintf( stderr, "Received explicit congestion notification.\n" );
	}
      }
    }

    if ( p.timestamp_reply != uint16_t(-1) ) {
      uint16_t now = timestamp16();
      double R = timestamp_diff( now, p.timestamp_reply );

      if ( R < 5000 ) { /* ignore large values, e.g. server was Ctrl-Zed */
	if ( !RTT_hit ) { /* first measurement */
	  SRTT = R;
	  RTTVAR = R / 2;
	  RTT_hit = true;
	} else {
	  const double alpha = 1.0 / 8.0;
	  const double beta = 1.0 / 4.0;
	  
	  RTTVAR = (1 - beta) * RTTVAR + ( beta * fabs( SRTT - R ) );
	  SRTT = (1 - alpha) * SRTT + ( alpha * R );
	}
      }
    }

    /* auto-adjust to remote host */
    has_remote_addr = true;
    last_heard = timestamp();

    if ( server ) { /* only client can roam */
      if ( !addreq(&remote_addr, &packet_remote_addr) ) {
        remote_addr = packet_remote_addr;
	remote_addr_len = sizeof( packet_remote_addr );
        // TODO
        //fprintf( stderr, "Server now attached to client at %s:%d\n",
        //         inet_ntoa( remote_addr.sin_addr ),
        //         ntohs( remote_addr.sin_port ) );
      }
    }
  }

  return p.payload; /* we do return out-of-order or duplicated packets to caller */
}

int Connection::port( void ) const
{
  struct sockaddr_storage local_addr;
  char portstr[6];
  socklen_t addrlen = sizeof( local_addr );

  if ( getsockname( sock, (sockaddr *)&local_addr, &addrlen ) < 0 ) {
    throw NetworkException( "getsockname", errno );
  }

  if ( getnameinfo( (const struct sockaddr*)&local_addr, addrlen, NULL, 0,
                    portstr, sizeof(portstr), NI_NUMERICSERV) < 0 ) {
    throw NetworkException( "getnameinfo", errno );
  }

  /* No error checking since getnameinfo() returns a valid numeric port. */
  return strtol( portstr, NULL, 10 );
}

uint64_t Network::timestamp( void )
{
  return frozen_timestamp();
}

uint16_t Network::timestamp16( void )
{
  uint16_t ts = timestamp() % 65536;
  if ( ts == uint16_t(-1) ) {
    ts++;
  }
  return ts;
}

uint16_t Network::timestamp_diff( uint16_t tsnew, uint16_t tsold )
{
  int diff = tsnew - tsold;
  if ( diff < 0 ) {
    diff += 65536;
  }
  
  assert( diff >= 0 );
  assert( diff <= 65535 );

  return diff;
}

uint64_t Connection::timeout( void ) const
{
  uint64_t RTO = lrint( ceil( SRTT + 4 * RTTVAR ) );
  if ( RTO < MIN_RTO ) {
    RTO = MIN_RTO;
  } else if ( RTO > MAX_RTO ) {
    RTO = MAX_RTO;
  }
  return RTO;
}

Connection::~Connection()
{
  if ( close( sock ) < 0 ) {
    throw NetworkException( "close", errno );
  }
}
