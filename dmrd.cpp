/*
	Pi-Star-compatible DMR server (MMDVM protocol),    
	Created April 2020, Michael Wagner, W9ZEP
	(c) 2020 Michael J Wagner

	This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details at 
	https://www.gnu.org/licenses

	This code assumes 32-bit compiler. Should work on 64-bit but I haven't tested it.
	
	This has been built and tested on GNU C++ on Centos Linux (Intel 32-bit) and Visual C++ 6.0 for Windows.

	To use the parrot, do a private call to your radio's ID

	TODO 

	save/restore database
	Test on big endian CPU.
	Test on 64-bit.
	DoS mitigation.

	HISTORY

	05-18-2020	major changes to findnode() and delete_node()
				assumes large memory server.
				no longer using std::map for anything
				node management optimized
				private call optimized
				version 0.17

	05-24-2020	more logging.
				version 0.18.

	06-28-2020	fixed bug in do_housekeeping()
				version 0.20

*/

#include "dmrd.h"

#define VERSION 0
#define RELEASE 20

//#define BIG_ENDIAN_CPU
#define LOW_DMRID 1000000			/* lowest acceptible DMR ID not including ESSID */
#define HIGH_DMRID 8000000			/* highest acceptible DMR ID not including ESSID */
#define MAX_TALK_GROUPS 10000		/* highest possible TG */
#define TAC_TG_START 100			/* the first default TAC group to make */
#define TAC_TG_END 109				/* the last default TAC group to make */
#define SCANNER_TG 777				/* when radios connect to this, they head the 'scanner' */
#define UNSUBSCRIBE_ALL_TG 4000
#define MAX_PASSWORD_SIZE 120
#define DEFAULT_HOUSEKEEPING_MINUTES 1
#define DEFAULT_PORT 62031

#define NODEID(SLOTID) ((SLOTID) & 0x7FFFFFFF)						/* strip off slot bit */
#define SLOTID(NODEID,SLOT) ((NODEID) | ((SLOT) ? 0x80000000 : 0))	/* make a slotid */
#define SLOT(SLOTID) (((SLOTID) & 0x80000000) ? 1 : 0)				/* get slot number 0 or 1 (for slot 1 and 2) */

int g_sock = -1;
int g_debug = 0;
int g_udp_port = DEFAULT_PORT;
char g_password[MAX_PASSWORD_SIZE];
int g_housekeeping_minutes = DEFAULT_HOUSEKEEPING_MINUTES;
dword volatile g_tick;		// ticks since server started, this will rollover
dword volatile g_sec;		// seconds since server started

#define inet_ntoa __use_my_inet_ntoa__

struct slot
{
	struct node		*node;				// parent node
	dword			slotid;
	dword			tg;					// subscribed talkgroup else 0
	slot			*prev, *next;		// talkgroup chain of subscribers
	dword			parrotstart;		// parrot start time
	int				parrotendcount;	
	memfile			*parrot;			// record parrot DMRD packets
	byte volatile	parrotseq;
};

struct node			// e.g, a pistar node
{
	dword			nodeid;				// full node ID with ESSID if present
	dword			dmrid;				// node ID without ESSID. If no ESSID, then identical to nodeid
	dword			salt;				// used for authentication
	sockaddr_in		addr;				// last known IP address
	dword			hitsec;				// last time heard
	slot			slots[2];			// two slots
	bool			bAuth;				// node has been authenticated

	node() {

		memset(this, 0, sizeof(*this));

		slots[0].node = this;
		slots[1].node = this;
	}
};

struct nodevector {

	dword			radioslot;			// slot where the radio of the same dmrid was heard

	struct node *sub[100];			// allocate for 100 ESSID

	nodevector() {

		memset (this, 0, sizeof(*this));
	}
};

nodevector * g_node_index [HIGH_DMRID-LOW_DMRID];		// large array to point to nodevectors

// used for parrot processing

struct parrot_exec
{
	sockaddr_in		addr;
	memfile			*file;

	parrot_exec() {

		file = NULL;
	}

	~parrot_exec() {

		delete file;
		file = NULL;
	}
};

//////////////////////////////////////////////////////////////////////////////////////////

struct talkgroup
{	
	dword		tg;					// talk group #
	dword		ownerslot;			// slotid of owner else 0
	dword		tick;				// clock tick (ms) of last audio packet from owner
	slot		*subscribers;		// active listeners

	talkgroup() {
		
		tg = 0;	
		ownerslot = 0;
		tick = 0;
		subscribers = NULL;
	}
};

talkgroup *g_talkgroups[MAX_TALK_GROUPS];

talkgroup *g_scanner;		// the scanner TG

//////////////////////////////////////////////////////////////////////////////////////////

std::string my_inet_ntoa (in_addr in)
{
	char buf[20];

	dword n = *(dword*)&in;

	sprintf (buf, "%03d.%03d.%03d.%03d", 
		(byte)(n),
		(byte)(n >> 8),
		(byte)(n >> 16),
		(byte)(n >> 24));

	return buf;
}

std::string slotid_str (dword slotid)
{
	char buf[20];

	sprintf (buf, "%u:%u", NODEID(slotid), SLOT(slotid)+1);

	return buf;
}

void dumphex (PCSTR pName, void const *p, int nSize)
{
	printf ("%s: ", pName);

	for (int i=0; i < nSize; i++) {

		printf ("%02X", ((BYTE*)p)[i]);
	}

	putchar ('\n');
}

bool select_rx (int sock, int wait_secs)
{
	fd_set read;

	FD_ZERO (&read);

	FD_SET (sock, &read);

	timeval t;

	t.tv_sec = wait_secs;
	t.tv_usec = 0;

	int ret = select (sock + 1, &read, NULL, NULL, &t);     

	if (ret == -1)
		return false;

	return !!ret;
}

int open_udp (int port)
{
	int err;

	int sock = socket(AF_INET, SOCK_DGRAM, 0);

	if (sock == -1) 
		return -1;

	int on = true;
	
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*) &on, sizeof(on));

	sockaddr_in addr;

	memset (&addr, 0, sizeof(addr));

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	if (bind (sock, (sockaddr*) &addr, sizeof(addr)) == -1) {

		err = errno;
		CLOSESOCKET(sock);
		errno = err;
		return -1;
	}

	// Allow broadcast

	int bArg = true;

	if (setsockopt (sock, SOL_SOCKET, SO_BROADCAST, (char*) &bArg, sizeof(bArg)) == -1) {

		err = errno;
		CLOSESOCKET(sock);
		errno = err;
		return -1;
	}

	return sock;
}

#ifdef WIN32
int pthread_create (pthread_t *th, const pthread_attr_t *pAttr, PTHREADPROC pProc, void *pArg)
{
	assert(th);

	unsigned hThread = 0;
	
	if (_beginthreadex (NULL, 0, pProc, pArg, 0, &hThread) == 0)
		return errno;

	*th = (pthread_t) hThread;		 

	return 0;
}
#endif

// SHA256 

#define SHA256_BLOCK_SIZE 32            // SHA256 outputs a 32 byte digest

typedef struct {
	BYTE data[64];
	DWORD datalen;
	u64 bitlen;
	DWORD state[8];
} SHA256_CTX;

#define ROTLEFT(a,b) (((a) << (b)) | ((a) >> (32-(b))))
#define ROTRIGHT(a,b) (((a) >> (b)) | ((a) << (32-(b))))

#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x,2) ^ ROTRIGHT(x,13) ^ ROTRIGHT(x,22))
#define EP1(x) (ROTRIGHT(x,6) ^ ROTRIGHT(x,11) ^ ROTRIGHT(x,25))
#define SIG0(x) (ROTRIGHT(x,7) ^ ROTRIGHT(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x,17) ^ ROTRIGHT(x,19) ^ ((x) >> 10))

static const DWORD k[64] = {
	
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_transform(SHA256_CTX *ctx, const BYTE data[])
{
	DWORD a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];

	for (i = 0, j = 0; i < 16; ++i, j += 4)
		m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);
	for ( ; i < 64; ++i)
		m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];
	f = ctx->state[5];
	g = ctx->state[6];
	h = ctx->state[7];

	for (i = 0; i < 64; ++i) {
		t1 = h + EP1(e) + CH(e,f,g) + k[i] + m[i];
		t2 = EP0(a) + MAJ(a,b,c);
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

static void sha256_init(SHA256_CTX *ctx)
{
	ctx->datalen = 0;
	ctx->bitlen = 0;
	ctx->state[0] = 0x6a09e667;
	ctx->state[1] = 0xbb67ae85;
	ctx->state[2] = 0x3c6ef372;
	ctx->state[3] = 0xa54ff53a;
	ctx->state[4] = 0x510e527f;
	ctx->state[5] = 0x9b05688c;
	ctx->state[6] = 0x1f83d9ab;
	ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(SHA256_CTX *ctx, const BYTE data[], size_t len)
{
	int i;

	for (i = 0; i < len; ++i) {
		ctx->data[ctx->datalen] = data[i];
		ctx->datalen++;
		if (ctx->datalen == 64) {
			sha256_transform(ctx, ctx->data);
			ctx->bitlen += 512;
			ctx->datalen = 0;
		}
	}
}

static void sha256_final(SHA256_CTX *ctx, BYTE hash[])
{
	int i;

	i = ctx->datalen;

	// Pad whatever data is left in the buffer.
	if (ctx->datalen < 56) {
		ctx->data[i++] = 0x80;
		while (i < 56)
			ctx->data[i++] = 0x00;
	}
	else {
		ctx->data[i++] = 0x80;
		while (i < 64)
			ctx->data[i++] = 0x00;
		sha256_transform(ctx, ctx->data);
		memset(ctx->data, 0, 56);
	}

	// Append to the padding the total message's length in bits and transform.
	ctx->bitlen += ctx->datalen * 8;
	ctx->data[63] = ctx->bitlen;
	ctx->data[62] = ctx->bitlen >> 8;
	ctx->data[61] = ctx->bitlen >> 16;
	ctx->data[60] = ctx->bitlen >> 24;
	ctx->data[59] = ctx->bitlen >> 32;
	ctx->data[58] = ctx->bitlen >> 40;
	ctx->data[57] = ctx->bitlen >> 48;
	ctx->data[56] = ctx->bitlen >> 56;
	sha256_transform(ctx, ctx->data);

	// Since this implementation uses little endian byte ordering and SHA uses big endian,
	// reverse all the bytes when copying the final state to the output hash.
	for (i = 0; i < 4; ++i) {
		hash[i]      = (ctx->state[0] >> (24 - i * 8)) & 0x000000ff;
		hash[i + 4]  = (ctx->state[1] >> (24 - i * 8)) & 0x000000ff;
		hash[i + 8]  = (ctx->state[2] >> (24 - i * 8)) & 0x000000ff;
		hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0x000000ff;
		hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0x000000ff;
		hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0x000000ff;
		hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0x000000ff;
		hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0x000000ff;
	}
}

byte * make_sha256_hash (void const *pSrc, int nSize, byte *dest, void const *pSalt, int nSaltSize)
{
	SHA256_CTX ctx;

	sha256_init (&ctx);

	sha256_update(&ctx, (byte*)pSrc, nSize);
	
	if (pSalt)
		sha256_update(&ctx, (byte*)pSalt, nSaltSize);

	sha256_final(&ctx, dest);

	return dest;	
}

bool IsOptionPresent (int argc, char **argv, PCSTR arg)
{
	for (int i=1; i < argc; i++) {

		if (strcmp(argv[i],arg)==0)
			return true;
	}

	return false;
}

void trim (std::string &s) {

	int x = s.size() - 1;

	while (x >= 0 && isspace(s[x]))
		s.erase(x--);
}

PCSTR skipspaces (PCSTR p, bool bSkipTabs, bool bSkipCtrl)
{
	while (*p) {

		if (*p == ' ') {

			p ++;
		}

		else if (bSkipCtrl && *p > 0 && *p < ' ') {

			p ++;
		}

		else if (bSkipTabs && *p == '\t') {

			p ++;
		}

		else
			break;
	}

	return p;
}

static void _init_process() 
{
	assert(sizeof(WORD) == 2);
	assert(sizeof(word) == 2);
	assert(sizeof(DWORD) == 4);
	assert(sizeof(dword) == 4);
	assert(sizeof(u64) == 8);
	setbuf(stdout,NULL);
}

void init_process()
{
	_init_process();

#ifdef WIN32

	WSADATA wsa; 
	WSAStartup (MAKEWORD(1,1), &wsa);
	_umask(0);

#else

	signal(SIGPIPE,SIG_IGN);
	//signal(SIGSEGV,SIG_IGN);
	//signal(SIGCHLD,SIG_IGN);
	signal(SIGCHLD,SIG_DFL);
	struct rlimit r;
	memset (&r, 0, sizeof(r));
	setrlimit(RLIMIT_CORE, &r);	// no core dumps
	umask(0);

#endif
}

void unsubscribe_from_group(slot *s);

void log (sockaddr_in *addr, PCSTR fmt, ...)
{
	int err = errno;
	
	int nerr = GetInetError();

	try {
		
		char temp[300];

		va_list marker;
		
		va_start (marker, fmt);

		time_t tt = time(NULL);

		tm *t = localtime(&tt);

		sprintf (temp, "%02d-%02d-%02d %02d:%02d:%02d %-15s ", t->tm_mon+1, t->tm_mday, t->tm_year % 100, t->tm_hour, t->tm_min, t->tm_sec, addr ? my_inet_ntoa(addr->sin_addr).c_str() : "000.000.000.000");

		vsprintf (temp + strlen(temp), fmt, marker);

		char *p = temp + strlen(temp) - 1;

		while (p >= temp && (*p == '\r' || *p == '\n'))
			*p-- = 0;

		puts (temp);
	}

	catch (...) {
	}

	errno = err;

	SetInetError (nerr);
}

word inline get2 (byte const *p)
{
	// network order is big endian
#ifdef BIG_ENDIAN_CPU
	return *(word*) p;
#else
	return ((word)p[0] << 8) + p[1];
#endif
}

dword inline get3 (byte const *p)
{
	// network order is big endian
	return (dword)p[0] << 16 | ((word)p[1] << 8) | p[2];
}

dword inline get4 (byte const *p)
{
	// network order is big endian
#ifdef BIG_ENDIAN_CPU
	return *(dword*) p;
#else
	return (dword)p[0] << 24 | (dword)p[1] << 16 | (word)p[2] << 8 | p[3];
#endif
}

void inline set3 (byte *p, dword n)
{
	// network order is big endian
	*p++ = n >> 16;
	*p++ = n >> 8;
	*p++ = n;
}

void inline set4 (byte *p, dword n)
{
	// network order is big endian
	*p++ = n >> 24;
	*p++ = n >> 16;
	*p++ = n >> 8;
	*p++ = n;
}

void show_packet (PCSTR title, char const *ip, byte const *pk, int sz, bool bShowDMRD=false) 
{
	if (g_debug) {

		printf ("%s %s size %d\n", title, ip, sz);

		for (int i=0; i < sz; i++) 
			printf ("%02X ", pk[i]);

		putchar ('\n');

		for (i=0; i < sz; i++) 
			printf ("%c", inrange(pk[i],32,127) ? pk[i] : '.');

		putchar ('\n');

		if (bShowDMRD && sz == 55 && memcmp(pk, "DMRD", 4)==0) {

			dword radioid = get3(pk + 5);	// radio ID

			dword tg = get3 (pk + 8);	// tg or private call peer

			dword nodeid = get4(pk + 11);	// the pistar ID

			dword streamid = get4(pk + 16);		// stream ID

			int flags = pk[15];		// start/stop stream and frame number and slot

			dword slotid = SLOTID(nodeid, flags & 0x80);

			if (g_debug)
				printf ("node %d slot %d radio %d group %d stream %08X flags %02X\n\n", nodeid, SLOT(slotid)+1, radioid, tg, streamid, flags);
		}

		putchar ('\n');
	}
}

void sendpacket (sockaddr_in addr, void const *p, int sz)
{
	show_packet ("TX", my_inet_ntoa(addr.sin_addr).c_str(), (byte const*)p, sz, true);

	sendto (g_sock, (char*)p, sz, 0, (sockaddr*)&addr, sizeof(addr));
}

node * findnode (dword nodeid, bool bCreateIfNecessary)
{
	node *n = NULL;

	nodeid = NODEID(nodeid);	// strip off possible slot bit

	dword dmrid, essid;

	if (nodeid > 0xFFFFFF) {	// has ESSID?

		dmrid = nodeid / 100;
		essid = nodeid % 100;
	}

	else {

		dmrid = nodeid;
		essid = 0;
	}

	if (!inrange(dmrid,LOW_DMRID,HIGH_DMRID)) 
		return NULL;

	int ix = dmrid-LOW_DMRID;

	if (!g_node_index[ix]) {

		g_node_index[ix] = new nodevector;
	}

 	if (!g_node_index[ix]->sub[essid]) {

		n = g_node_index[ix]->sub[essid] = new node;

		n->nodeid = nodeid;
		n->dmrid = dmrid;

		n->slots[0].slotid = SLOTID(nodeid,0);
		n->slots[1].slotid = SLOTID(nodeid,1);
	}

	else {

		n = g_node_index[ix]->sub[essid];
	}

	return n;
}

void delete_node (dword nodeid)
{
	nodeid = NODEID(nodeid);	// strip off possible slot

	node *n = NULL;

	int dmrid, essid;

	if (nodeid > 0xFFFFFF) {

		dmrid = nodeid / 100;
		essid = nodeid % 100;
	}

	else {

		dmrid = nodeid;
		essid = 0;
	}

	if (inrange(dmrid,LOW_DMRID,HIGH_DMRID)) {

		int ix = dmrid-LOW_DMRID;

		if (g_node_index[ix]) {

			node *n = g_node_index[ix]->sub[essid];

			if (n) {

				log (&n->addr, "Delete node %d\n", nodeid);

				unsubscribe_from_group (&n->slots[0]);

				unsubscribe_from_group (&n->slots[1]);

				g_node_index[ix]->sub[essid] = NULL;

				bool bNodes = false;

				for (int i=0; i < 100; i++) {

					if (g_node_index[ix]->sub[essid]) {

						bNodes = true;
						break;
					}
				}

				if (!bNodes) {

					delete g_node_index[ix];
					
					g_node_index[ix] = NULL;
				}

				delete n;
			}
		}
	}
}

slot * findslot (int slotid, bool bCreateIfNecessary)
{
	node *n = findnode (NODEID(slotid), bCreateIfNecessary);

	if (!n)
		return NULL;

	return &n->slots[SLOT(slotid)];
}

talkgroup * findgroup (dword tg, bool bCreateIfNecessary)
{
	if (!inrange(tg,1,MAX_TALK_GROUPS-1))
		return NULL;

	if (!g_talkgroups[tg] && bCreateIfNecessary) {

		g_talkgroups[tg] = new talkgroup;

		g_talkgroups[tg]->tg = tg;
	}

	return g_talkgroups[tg];
}

void _dump_groups(std::string &ret)
{
	char temp[200];

	for (int i=0; i < MAX_TALK_GROUPS; i++) {

		talkgroup const *g = g_talkgroups[i];

		sprintf (temp, "TALKGROUP %d owner %d slot %d head %p %d\n", g->tg, NODEID(g->ownerslot), SLOT(g->ownerslot)+1, g->subscribers, g->subscribers ? g->subscribers->node->nodeid : 0);

		ret += temp;

		slot *s = g->subscribers;

		while (s) {

			sprintf (temp, "\t%p node %d slot %d prev %p next %p\n", s, s->node->nodeid, SLOT(s->slotid)+1, s->prev, s->next);

			ret += temp;

			s = s->next;
		}
	}
}

void dump_groups()
{
	if (g_debug) {

		std::string str;

		puts (str.c_str());
	}
}

void _dump_nodes(std::string &ret)
{
	char temp[200];

	sprintf (temp, "Sec %d tick %u\n", g_sec, g_tick);

	ret += temp;

	for (int ix=0; ix < HIGH_DMRID - LOW_DMRID; ix++) {

		if (g_node_index[ix]) {

			sprintf (temp, "Node vector %d, radioslot %d\n", ix + LOW_DMRID, g_node_index[ix]->radioslot);

			ret += temp;

			for (int essid=0; essid < 100; essid++) {

				node const *n = g_node_index[ix]->sub[essid];

				if (n) {

					sprintf (temp, "\t%s ID %d dmrid %d auth %d sec %u\n", my_inet_ntoa(n->addr.sin_addr).c_str(), n->nodeid, n->dmrid, n->bAuth, n->hitsec);

					ret += temp;

					if (n->slots[0].tg) {

						sprintf (temp, "\t\tS1 TG %d\n", n->slots[0].tg);
						ret += temp;
					}

					if (n->slots[1].tg) {
						
						sprintf (temp, "\t\tS2 TG %d\n", n->slots[1].tg);
						ret += temp;
					}
				}
			}
		}
	}
}

void dump_nodes()
{
	if (g_debug) {

		std::string str;

		puts (str.c_str());
	}
}

void unsubscribe_from_group(slot *s)
{
	if (s->tg) {

		log (&s->node->addr, "Unsubscribe group %u node %d slot %d from talkgroup %d\n", s->tg, s->node->nodeid, SLOT(s->slotid)+1);

		talkgroup *g = findgroup (s->tg, false);

		// remove from subscriber list

		if (g) {

			if (g->ownerslot == s->slotid)		// if owner, release ownership
				g->ownerslot = 0;

			if (s->prev)
				s->prev->next = s->next;

			if (s->next)
				s->next->prev = s->prev;

			if (g->subscribers == s)
				g->subscribers = s->next;
		}

		s->next = s->prev = NULL;

		s->tg = 0;

		dump_groups ();
	}
}

void subscribe_to_group(slot *s, talkgroup *g)
{
	if (s->tg != g->tg) {

		log (&s->node->addr, "Subscribe group %u node %d slot %d to talkgroup %d\n", g->tg, s->node->nodeid, SLOT(s->slotid)+1);

		unsubscribe_from_group(s);
			
		// insert at head of subscriber list for this group

		s->tg = g->tg;
		s->prev = NULL;
		s->next = g->subscribers;

		if (s->next)
			s->next->prev = s;

		g->subscribers = s;

		dump_groups ();
	}
}

void do_housekeeping()
{
	dword t = g_sec;

	dword starttick = g_tick;

	log (NULL, "Housekeeping, tick %u\n", starttick);

	// delete any inactive nodes 

	int active = 0, dropped_nodes = 0, radios = 0, dropped_radios = 0;

	for (int ix=0; ix < HIGH_DMRID - LOW_DMRID; ix++) {

		for (int essid=0; g_node_index[ix] && essid < 100; essid++) {	// since g_node_index[ix] can go NULL in delete_node() we have to keep checking it every iteration

			node const *n = g_node_index[ix]->sub[essid];

			if (n) {

				if (g_sec - n->hitsec >= 60) {		// node must at least ping once a minute

					dropped_nodes ++;

					delete_node (n->nodeid);
				}

				else {

					active ++;
				}
			}
		}
	}
	
	log (NULL, "Done - %u secs, %u active nodes, %u dropped nodes, %d radios, %d dropped radios, %u ticks\n", g_sec, active, dropped_nodes, radios, dropped_radios, g_tick - starttick);
}

void swapbytes (byte *a, byte *b, int sz)
{
	while (sz--) 
		swap (*a++, *b++);
}

// Time thread. We don't use time() or any other time function in the C/C++ library for the "DMRD" packets
// because they are expensive, and we want the highest performance.
// This counter does not need to be perfectly accurate.

PTHREAD_PROC(time_thread_proc) 
{
	for (;;) {

		Sleep (50);

		g_tick += 50;

		if (!(g_tick % 1000))
			g_sec ++;
	}

	return 0;
}

// thread to playback a parrot recording

PTHREAD_PROC(parrot_playback_thread_proc)
{
	parrot_exec *e = (parrot_exec*) threadcookie;

	e->file->Seek(0);

	byte buf[55];

	Sleep (1000);		// delay for a second

	while (e->file->Read (buf, 55)) {

		sendpacket (e->addr, buf, 55);

		Sleep (20);
	}

	delete e;

	return 0;
}

// handle all received packets

void handle_rx (sockaddr_in &addr, byte *pk, int pksize)
{
	if (pksize == 55 && memcmp(pk, "DMRD", 4)==0) {		// DMR radio audio payload

		dword const radioid = get3(pk + 5);	// radio ID

		dword const tg = get3 (pk + 8);	// tg or private call peer

		dword const nodeid = get4(pk + 11);	// the pistar ID

		dword const streamid = get4(pk + 16);		// stream ID

		int const flags = pk[15];		// start/stop stream and frame number and slot

		bool const bStartStream = (flags & 0x23) == 0x21;

		bool const bEndStream = (flags & 0x23) == 0x22;

		bool const bPrivateCall = (flags & 0x40) == 0x40;

		dword const slotid = SLOTID(nodeid, flags & 0x80);

		if (g_debug)
			printf ("node %d slot %d radio %d group %d stream %08X flags %02X\n\n", nodeid, SLOT(slotid)+1, radioid, tg, streamid, flags);

		slot *s = findslot (slotid, true);

		if (!s) {

			log (&addr, "Slotid %s not found for DMRD\n", slotid_str(slotid).c_str());
			return;
		}

		if (!s->node->bAuth) {		// node hasn't been authenticated?

			log (&addr, "Node %d not authenticated for DMRD\n", nodeid);
			return;
		}

		if (getinaddr(s->node->addr) != getinaddr(addr)) {	// coming from a bogus IP?

			log (&addr, "Node %d invalid IP DMRD. Should be %s\n", nodeid, my_inet_ntoa(addr.sin_addr).c_str());
			return;
		}

		s->node->addr = addr;	// update IP

		s->node->hitsec = g_sec;

		if (inrange(radioid,LOW_DMRID,HIGH_DMRID) && g_node_index[radioid-LOW_DMRID])
			g_node_index[radioid-LOW_DMRID]->radioslot = slotid;

		if (tg == UNSUBSCRIBE_ALL_TG) {		// unsubscribe only?

			if (bStartStream) {

				log (&addr, "Unsubscribe all, slotid %s\n", slotid_str(slotid).c_str());
				unsubscribe_from_group (s);
			}

			return;
		}

		if (bPrivateCall) {		// private call?

			if (tg == radioid) {	// if to self, then this is a parrot

				if (bEndStream) {	// done?

					log (&addr, "Parrot stream end on nodeid %u slotid %s radioid %u\n", nodeid, slotid_str(slotid).c_str(), radioid);

					// is the parrot really running?

					if (s->parrot) {

						s->parrot->Write (pk, pksize);

						// hand it off to a parrot playback thread

						parrot_exec *e = new parrot_exec;

						e->addr = s->node->addr;
						e->file = s->parrot;
						s->parrot = NULL;

						pthread_t th;

						pthread_create (&th, NULL, parrot_playback_thread_proc, e);		// the thread will echo the packets back
					}
				}

				else {

					if (bStartStream) {	// start parrot?

						log (&addr, "Parrot stream start on nodeid %u slotid %s radioid %u\n", nodeid, slotid_str(slotid).c_str(), radioid);

						unsubscribe_from_group (s);

						// My pistar always has two flags==0xE1 packets, so it's possible we've already started the parrot

						if (!s->parrot) {	

							// fixme: make sure to purge this in the housekeeping if it's hanging around

							s->parrot = new memfile;
							s->parrotseq ++;
							s->parrotstart = g_sec;
						}
					}

					if (s->parrot && g_sec - s->parrotstart < 6) {		// limit duration

						s->parrot->Write (pk, pksize);
					}
				}
			}

			else {		// private call

				// see if we can locate the radio

				unsubscribe_from_group (s);

				if (bStartStream) {

					log (&addr, "Private stream start, from radioid %u to radioid %u\n", radioid, tg);
				}

				else if (bEndStream) {

					log (&addr, "Private stream end, from radioid %u to radioid %u\n", radioid, tg);
				}

				if (inrange(tg,LOW_DMRID,HIGH_DMRID)) {
				
					if (g_node_index[tg-LOW_DMRID]) {

						dword slotid = g_node_index[tg-LOW_DMRID]->radioslot;

						slot const *dest = findslot (slotid, false);

						if (dest) {

							if (bStartStream || bEndStream) {

								log (&addr, "Private stream dest slotid %s found, from radioid %u to radioid %u\n", slotid_str(slotid).c_str(), radioid, tg);
							}

							if (SLOT(slotid))
								pk[15] |= 0x80;

							else
								pk[15] &= 0x7F;

							sendpacket (dest->node->addr, pk, pksize);
						}

						else {

							if (bStartStream || bEndStream) {

								log (&addr, "Private stream dest slotid %s not found, from radioid %u to radioid %u\n", slotid_str(slotid).c_str(), radioid, tg);
							}
						}
					}

					else {

						if (bStartStream || bEndStream) {

							log (&addr, "Private stream dest radioid not in node index, from radioid %u to radioid %u\n", radioid, tg);
						}
					}
				}

				else {

					if (bStartStream || bEndStream) {

						log (&addr, "Private stream dest radioid out of range, from radioid %u to radioid %u\n", radioid, tg);
					}
				}
			}
		}

		else {	// talkgroup 

			talkgroup *g = findgroup (tg, false);

			if (g) {	// group exists?

				if (s->tg != tg) {	// not already subscribed?
				
					subscribe_to_group(s, g);
				}

				if (tg != SCANNER_TG) {

					if (g->ownerslot && g_tick - g->tick >= 1500) {	// current group owner timed out?

						log (&addr, "Timeout group %u, slotid %s", tg, slotid_str(g->ownerslot).c_str());

						g->ownerslot = 0;
					}

					if (bStartStream && !g->ownerslot) {

						log (&addr, "Take group %u, nodeid %u slotid %s radioid %u", tg, nodeid, slotid_str(slotid).c_str(), radioid);
							
						g->ownerslot = slotid;

						g->tick = g_tick;
					}

					else if (bEndStream && g->ownerslot == slotid) {

						log (&addr, "Drop group %u, nodeid %u slotid %s radioid %u", tg, nodeid, slotid_str(slotid).c_str(), radioid);

						g->ownerslot = 0;
					}
					
					if (slotid == g->ownerslot) {

						g->tick = g_tick;

						// relay packet to subscribers

						slot const *dest = g->subscribers;

						while (dest) {

							if (dest->slotid != slotid) {	// don't send packet back to sender

								if (SLOT(dest->slotid))
									pk[15] |= 0x80;

								else
									pk[15] &= 0x7F;

								sendpacket (dest->node->addr, pk, pksize);
							}

							dest = dest->next;
						}
					}

					if (g_scanner->ownerslot && g_tick - g_scanner->tick >= 1500) {

						log (&addr, "Timeout scanner, nodeid %u slotid %s radioid %u", nodeid, slotid_str(slotid).c_str(), radioid);

						g_scanner->ownerslot = 0;
					}

					// if slot owns scanner and end of stream, or owner timed out, drop ownership

					if (s->slotid == g_scanner->ownerslot && bEndStream) {

						log (&addr, "Drop scanner, nodeid %u slotid %s radioid %u", nodeid, slotid_str(slotid).c_str(), radioid);

						g_scanner->ownerslot = 0;
					}

					// if nobody owns the scanner, and this isn't the end of a stream, take tx ownership

					if (!g_scanner->ownerslot && !bEndStream) {

						log (&addr, "Take scanner, nodeid %u slotid %s radioid %u", nodeid, slotid_str(slotid).c_str(), radioid);

						g_scanner->ownerslot = s->slotid;

						g_scanner->tick = g_tick;
					}

					// if current slot is current scanner stream, relay the packet to scanner subscribers

					if (s->slotid == g_scanner->ownerslot) {

						g_scanner->tick = g_tick;

						slot const *dest = g_scanner->subscribers;

						while (dest) {

							if (SLOT(dest->slotid))
								pk[15] |= 0x80;

							else
								pk[15] &= 0x7F;

							sendpacket (dest->node->addr, pk, pksize);
	
							dest = dest->next;
						}
					}
				}
			}

			else {

				if (bStartStream)
					log (&addr, "Nodeid %u keyup on non-existent group %u", nodeid, tg);

				unsubscribe_from_group (s);
			}
		}
	}

	else if (pksize == 8 && memcmp(pk, "RPTL", 4)==0) {		// login

		dword nodeid = get4(pk + 4);

		log (&addr, "RPTL node %d\n", nodeid);

		node *n = findnode (nodeid, false);

		if (n) {		// node exists?

			// if already authenticated at a different IP then reject

			if (n->bAuth && getinaddr(addr) != getinaddr(n->addr)) {

				log (&addr, "Node %d already logged in at %s\n", nodeid, my_inet_ntoa(n->addr.sin_addr).c_str());
				return;
			}
		}

		if (!n) {

			n = findnode (nodeid, true);
		}

		n->hitsec = g_sec;

		if (!getinaddr(n->addr)) {

			n->addr = addr;
		}

		n->salt = ((dword)rand() << 16) ^ g_tick;	// reasonably random salt for RPTK authentication

		memcpy (pk, "RPTACK", 6);

		*(dword*)(pk + 6) = n->salt;

		sendpacket (addr, pk, 10);
	}

	else if (pksize == 40 && memcmp(pk, "RPTK", 4)==0) {		// authentication data

		dword nodeid = get4(pk + 4);

		log (&addr, "RPTK node %d\n", nodeid);

		node *n = findnode(nodeid, false);

		if (!n) {

			log (&addr, "Node %d not found for RPTK", nodeid);
			return;
		}

		if (getinaddr(n->addr) != getinaddr(addr)) {

			log (&addr, "Invalid RPTK IP address for node %d, should be %s\n", nodeid, my_inet_ntoa(n->addr.sin_addr).c_str());
			return;
		}

		n->hitsec = g_sec;

		if (!n->bAuth) {

			byte const * remotehash = pk + 8;

			byte localhash[32];

			char temp[MAX_PASSWORD_SIZE + 10];
			
			*(dword*)temp = n->salt;

			strcpy (temp + sizeof(n->salt), g_password);

			make_sha256_hash (temp, sizeof(n->salt) + strlen(g_password), localhash, NULL, 0);

			if (memcmp(localhash, remotehash, 32)==0) {

				n->bAuth = true;

				n->addr = addr;
			}
		}

		memcpy (pk, n->bAuth ? "RPTACK" : "MSTNAK", 6);

		if (!n->bAuth)
			log (&addr, "Authentication failed");

		set4 (pk + 6, nodeid);

		sendpacket (addr, pk, 10);
	}

	else if (pksize == 302 && memcmp(pk, "RPTC", 4)==0) {		// node description stuff, like callsign and location

		dword nodeid = get4(pk + 4);

		log (&addr, "RPTC node %d\n", nodeid);

		node *n = findnode (nodeid, false);

		if (!n) {

			log (&addr, "Node %d not found for RPTC", nodeid);
			return;
		}

		if (getinaddr(n->addr) != getinaddr(addr)) {

			log (&addr, "Invalid RPTC IP address for node %d, should be %s\n", nodeid, my_inet_ntoa(n->addr.sin_addr).c_str());
			return;
		}

		n->hitsec = g_sec;

		memcpy (pk, "RPTACK", 6);
		set4(pk + 6, nodeid);
		sendpacket (addr, pk, 10);
	}

	else if (pksize == 11 && memcmp(pk, "RPTPING", 7)==0) {

		dword nodeid = get4(pk + 7);

		node *n = findnode (nodeid, false);

		if (n && n->bAuth && getinaddr(addr) == getinaddr(n->addr)) {

			n->hitsec = g_sec;
			memcpy (pk, "MSTPONG", 7);
			set4 (pk+7, nodeid);
			sendpacket (addr, pk, 11);
		}

		else {

			memcpy (pk, "MSTNAK", 6);
			set4 (pk+6, nodeid);
			sendpacket (addr, pk, 10);
		}
	}

	else if (pksize == 9 && memcmp(pk, "RPTCL", 5)==0) {		// remove all trace of node

		dword nodeid = get4(pk + 5);

		log (&addr, "RPTCL node %d\n", nodeid);

		node *n = findnode (nodeid, false);

		if (!n) {

			log (&addr, "Node %d doesn't exist for RPTCL", nodeid);
			return;
		}

		if (getinaddr(addr) == getinaddr(n->addr)) {
	
			delete_node (nodeid);
		}

		else {

			log (&addr, "Invalid RPTCL IP address for node %d, should be %s\n", nodeid, my_inet_ntoa(n->addr.sin_addr).c_str());
		}
	}

	else if (pksize >= 5 && memcmp(pk, "/STAT", 5)==0) {		// return status to local query

		char temp[500];

		std::string str;

		_dump_nodes(str);

		memset (temp, 0, sizeof(temp));

		strncpy ((char*)temp, str.c_str(), sizeof(temp)-1);

		sendpacket (addr, temp, strlen((char*)temp));
	}

	dump_groups();
}

void run ()
{
	dword g_last_housekeeping_sec = 0;

	dword seq = 1;

	for (;;) {

		if (select_rx(g_sock, 1)) {

			byte buf[1000];

			sockaddr_in addr;

			socklen_t addrlen = sizeof(addr);

			int sz = recvfrom (g_sock, (char*) buf, sizeof(buf), 0, (sockaddr*)&addr, &addrlen);

			if (sz > 0) {

				char ip[50];
				
				strcpy (ip, my_inet_ntoa (addr.sin_addr).c_str());

				if (g_debug) {

					char temp[100];

					sprintf (temp, "RX%u", seq++);

					show_packet (temp, ip, buf, sz);
				}

				handle_rx (addr, buf, sz);
			}

			else if (sz < 1) {

				int err = GetInetError ();

				log (&addr, "recvfrom error %d\n", err);

				Sleep (50);
			}
		}

		if (g_sec - g_last_housekeeping_sec >= g_housekeeping_minutes * 60) {

			do_housekeeping();

			g_last_housekeeping_sec = g_sec;
		}
	}
}

// query status from locally running server

bool show_running_status()
{
	int sock;

	if ((sock = open_udp(62111)) == -1) {

		log (NULL, "Failed to open UDP port (%d)\n", GetInetError());
		return 1;
	}

	sockaddr_in addr;

	memset (&addr, 0, sizeof(addr));

	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_udp_port);
#ifdef WIN32
	addr.sin_addr.S_un.S_addr = inet_addr("127.0.0.1");
#else
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");
#endif

	if (sendto (sock, "/STAT", 5, 0, (sockaddr*)&addr, sizeof(addr)) == -1) {

		printf ("sendto() failed (%d)\n", GetInetError());
		CLOSESOCKET(sock);
		return false;
	}

	if (!select_rx (sock, 5)) {

		puts ("No reply from server");
		CLOSESOCKET(sock);
		return false;
	}

	char buf[1001];

	memset (buf, 0, sizeof(buf));

	int sz = recvfrom (sock, (char*) buf, sizeof(buf)-1, 0, NULL, 0);

	if (sz == -1) {

		printf ("recvfrom() failed (%d)\n", GetInetError());
		CLOSESOCKET(sock);
		return false;
	}

	puts (buf);

	CLOSESOCKET(sock);

	return true;
}

void process_config_file()
{
	config_file c;

	if (c.load ("/etc/dmrd.conf")) {

		strcpy (g_password, c.getstring ("security","password",g_password).c_str());
		g_udp_port = c.getint ("general","udp_port", g_udp_port);
		g_debug = c.getint("debug", "level", g_debug);
		g_housekeeping_minutes = c.getint ("general","housekeeping_minutes", g_housekeeping_minutes);
	}

	printf ("Config: debug %d, port %d, password %s, housekeeping minutes %d nodesize %d\n\n",
				g_debug, g_udp_port, g_password, g_housekeeping_minutes, sizeof(node));

}

int main(int argc, char **argv)
{
	init_process();

	puts ("\nCrazy Horse, Pi-Star Compatible (MMDVM Protcol) DMR Server");
	printf ("Version %d.%02d (%s)\n", VERSION, RELEASE, __DATE__" "__TIME__);
	puts ("(c) 2020 Michael J Wagner (W9ZEP)\n");

	if (IsOptionPresent(argc,argv,"--help"))
		return 0;

	srand (time(NULL));

	strcpy (g_password, "passw0rd");

	process_config_file();

	if (IsOptionPresent (argc, argv, "-d"))
		g_debug = true;

	if (IsOptionPresent(argc,argv,"-s"))		// show running server's status, then exit?
		return !show_running_status () ? 0 : 1;

#if 0
	puts ("This program is free software: you can redistribute it and/or modify");
    puts ("it under the terms of the GNU General Public License as published by");
    puts ("the Free Software Foundation, version 3 of the License.\n");

    puts ("This program is distributed in the hope that it will be useful,");
    puts ("but WITHOUT ANY WARRANTY; without even the implied warranty of");
    puts ("MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the");
    puts ("GNU General Public License for more details at");
	puts ("https://www.gnu.org/licenses\n");
#endif

	// make the talkgroups

	g_scanner = findgroup (SCANNER_TG, true);

	for (int i=TAC_TG_START; i <= TAC_TG_END; i++) 
		findgroup (i, true);

	// open the UDP port

	if ((g_sock = open_udp(g_udp_port)) == -1) {

		log (NULL, "Failed to open UDP port (%d)\n", GetInetError());
		return 1;
	}

	// create time thread

	pthread_t th;

	pthread_create (&th, NULL, time_thread_proc, NULL);

	// and begin...

	run();
		    
	return 0;	   
}



