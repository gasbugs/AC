// server.cpp: little more than enhanced multicaster
// runs dedicated or as client coroutine

#include "pch.h"

#ifdef WIN32
#include <io.h>
#else
#include <unistd.h>
#define _dup    dup
#define _fileno fileno
#endif

#include "cube.h"
#include "servercontroller.h"

#define DEBUGCOND (true)

void resetmap(const char *newname, int newmode, int newtime = -1, bool notify = true);
void disconnect_client(int n, int reason = -1);
bool refillteams(bool now = false, bool notify = true);
void changeclientrole(int client, int role, char *pwd = NULL, bool force=false);

servercontroller *svcctrl = NULL;
struct log *logger = NULL;

#define valid_flag(f) (f >= 0 && f < 2)

#define SERVERMAP_PATH          "packages/maps/servermaps/"
#define SERVERMAP_PATH_BUILTIN  "packages/maps/"
#define SERVERMAP_PATH_INCOMING "packages/maps/servermaps/incoming/"

static const int DEATHMILLIS = 300;

enum { GE_NONE = 0, GE_SHOT, GE_EXPLODE, GE_HIT, GE_AKIMBO, GE_RELOAD, GE_SUICIDE, GE_PICKUP };
enum { ST_EMPTY, ST_LOCAL, ST_TCPIP };

int mastermode = MM_OPEN;
int verbose = 0;
string demopath, voteperm;

struct shotevent
{
    int type;
    int millis, id;
    int gun;
    float from[3], to[3];
};

struct explodeevent
{
    int type;
    int millis, id;
    int gun;
};

struct hitevent
{
    int type;
    int target;
    int lifesequence;
    union
    {
        int info;
        float dist;
    };
    float dir[3];
};

struct suicideevent
{
    int type;
};

struct pickupevent
{
    int type;
    int ent;
};

struct akimboevent
{
    int type;
    int millis, id;
};

struct reloadevent
{
    int type;
    int millis, id;
    int gun;
};

union gameevent
{
    int type;
    shotevent shot;
    explodeevent explode;
    hitevent hit;
    suicideevent suicide;
    pickupevent pickup;
    akimboevent akimbo;
    reloadevent reload;
};

template <int N>
struct projectilestate
{
    int projs[N];
    int numprojs;

    projectilestate() : numprojs(0) {}

    void reset() { numprojs = 0; }

    void add(int val)
    {
        if(numprojs>=N) numprojs = 0;
        projs[numprojs++] = val;
    }

    bool remove(int val)
    {
        loopi(numprojs) if(projs[i]==val)
        {
            projs[i] = projs[--numprojs];
            return true;
        }
        return false;
    }
};

struct clientstate : playerstate
{
    vec o;
    int state;
    int lastdeath, lastspawn, lifesequence;
    int lastshot;
    projectilestate<8> grenades;
    int akimbos, akimbomillis;
    int flagscore, frags, teamkills, deaths, shotdamage, damage;

    clientstate() : state(CS_DEAD) {}

    bool isalive(int gamemillis)
    {
        return state==CS_ALIVE || (state==CS_DEAD && gamemillis - lastdeath <= DEATHMILLIS);
    }

    bool waitexpired(int gamemillis)
    {
        int wait = gamemillis - lastshot;
        loopi(NUMGUNS) if(wait < gunwait[i]) return false;
        return true;
    }

    void reset()
    {
        state = CS_DEAD;
        lifesequence = -1;
        grenades.reset();
        akimbos = 0;
        akimbomillis = 0;
        flagscore = frags = teamkills = deaths = shotdamage = damage = 0;
        respawn();
    }

    void respawn()
    {
        playerstate::respawn();
        o = vec(-1e10f, -1e10f, -1e10f);
        lastdeath = 0;
        lastspawn = -1;
        lastshot = 0;
        akimbos = 0;
        akimbomillis = 0;
    }
};

struct savedscore
{
    string name;
    uint ip;
    int frags, flagscore, deaths, teamkills, shotdamage, damage;

    void save(clientstate &cs)
    {
        frags = cs.frags;
        flagscore = cs.flagscore;
        deaths = cs.deaths;
        teamkills = cs.teamkills;
        shotdamage = cs.shotdamage;
        damage = cs.damage;
    }

    void restore(clientstate &cs)
    {
        cs.frags = frags;
        cs.flagscore = flagscore;
        cs.deaths = deaths;
        cs.teamkills = teamkills;
        cs.shotdamage = shotdamage;
        cs.damage = damage;
    }
};

static vector<savedscore> scores;

struct client                   // server side version of "dynent" type
{
    int type;
    int clientnum;
    ENetPeer *peer;
    string hostname;
    string name, team;
    int skin;
    int vote;
    int role;
    int connectmillis;
    bool isauthed; // for passworded servers
    bool timesync;
    int gameoffset, lastevent, lastvotecall;
    int demoflags;
    clientstate state;
    vector<gameevent> events;
    vector<uchar> position, messages;
    string lastsaytext;
    int saychars, lastsay, spamcount;
    int at3_score, at3_lastforce, lastforce;
    bool at3_dontmove;
    int spawnindex;
    int salt;

    gameevent &addevent()
    {
        static gameevent dummy;
        if(events.length()>100) return dummy;
        return events.add();
    }

    void mapchange()
    {
        vote = VOTE_NEUTRAL;
        state.reset();
        events.setsizenodelete(0);
        timesync = false;
        lastevent = 0;
        at3_lastforce = 0;
    }

    void reset()
    {
        name[0] = team[0] = demoflags = 0;
        skin = 0;
        position.setsizenodelete(0);
        messages.setsizenodelete(0);
        isauthed = false;
        role = CR_DEFAULT;
        lastvotecall = 0;
        lastsaytext[0] = '\0';
        saychars = 0;
        lastforce = 0;
        spawnindex = -1;
        mapchange();
    }

    void zap()
    {
        type = ST_EMPTY;
        role = CR_DEFAULT;
        isauthed = false;
    }
};

vector<client *> clients;

bool valid_client(int cn)
{
    return clients.inrange(cn) && clients[cn]->type != ST_EMPTY;
}

struct ban
{
	ENetAddress address;
	int millis;
};

vector<ban> bans;

struct worldstate
{
    enet_uint32 uses;
    vector<uchar> positions, messages;
};

vector<worldstate *> worldstates;

void cleanworldstate(ENetPacket *packet)
{
   loopv(worldstates)
   {
       worldstate *ws = worldstates[i];
       if(packet->data >= ws->positions.getbuf() && packet->data <= &ws->positions.last()) ws->uses--;
       else if(packet->data >= ws->messages.getbuf() && packet->data <= &ws->messages.last()) ws->uses--;
       else continue;
       if(!ws->uses)
       {
           delete ws;
           worldstates.remove(i);
       }
       break;
   }
}

int bsend = 0, brec = 0, laststatus = 0, servmillis = 0, lastfillup = 0;

void recordpacket(int chan, void *data, int len);

void sendpacket(int n, int chan, ENetPacket *packet, int exclude = -1)
{
    if(n<0)
    {
        recordpacket(chan, packet->data, (int)packet->dataLength);
        loopv(clients) if(i!=exclude && (clients[i]->type!=ST_TCPIP || clients[i]->isauthed)) sendpacket(i, chan, packet);
        return;
    }
    switch(clients[n]->type)
    {
        case ST_TCPIP:
        {
            enet_peer_send(clients[n]->peer, chan, packet);
            bsend += (int)packet->dataLength;
            break;
        }

        case ST_LOCAL:
            localservertoclient(chan, packet->data, (int)packet->dataLength);
            break;
    }
}

static bool reliablemessages = false;

bool buildworldstate()
{
    static struct { int posoff, msgoff, msglen; } pkt[MAXCLIENTS];
    worldstate &ws = *new worldstate;
    loopv(clients)
    {
        client &c = *clients[i];
        if(c.type!=ST_TCPIP || !c.isauthed) continue;
        if(c.position.empty()) pkt[i].posoff = -1;
        else
        {
            pkt[i].posoff = ws.positions.length();
            loopvj(c.position) ws.positions.add(c.position[j]);
        }
        if(c.messages.empty()) pkt[i].msgoff = -1;
        else
        {
            pkt[i].msgoff = ws.messages.length();
            ucharbuf p = ws.messages.reserve(16);
            putint(p, SV_CLIENT);
            putint(p, c.clientnum);
            putuint(p, c.messages.length());
            ws.messages.addbuf(p);
            loopvj(c.messages) ws.messages.add(c.messages[j]);
            pkt[i].msglen = ws.messages.length()-pkt[i].msgoff;
        }
    }
    int psize = ws.positions.length(), msize = ws.messages.length();
    if(psize) recordpacket(0, ws.positions.getbuf(), psize);
    if(msize) recordpacket(1, ws.messages.getbuf(), msize);
    loopi(psize) { uchar c = ws.positions[i]; ws.positions.add(c); }
    loopi(msize) { uchar c = ws.messages[i]; ws.messages.add(c); }
    ws.uses = 0;
    loopv(clients)
    {
        client &c = *clients[i];
        if(c.type!=ST_TCPIP || !c.isauthed) continue;
        ENetPacket *packet;
        if(psize && (pkt[i].posoff<0 || psize-c.position.length()>0))
        {
            packet = enet_packet_create(&ws.positions[pkt[i].posoff<0 ? 0 : pkt[i].posoff+c.position.length()],
                                        pkt[i].posoff<0 ? psize : psize-c.position.length(),
                                        ENET_PACKET_FLAG_NO_ALLOCATE);
            sendpacket(c.clientnum, 0, packet);
            if(!packet->referenceCount) enet_packet_destroy(packet);
            else { ++ws.uses; packet->freeCallback = cleanworldstate; }
        }
        c.position.setsizenodelete(0);

        if(msize && (pkt[i].msgoff<0 || msize-pkt[i].msglen>0))
        {
            packet = enet_packet_create(&ws.messages[pkt[i].msgoff<0 ? 0 : pkt[i].msgoff+pkt[i].msglen],
                                        pkt[i].msgoff<0 ? msize : msize-pkt[i].msglen,
                                        (reliablemessages ? ENET_PACKET_FLAG_RELIABLE : 0) | ENET_PACKET_FLAG_NO_ALLOCATE);
            sendpacket(c.clientnum, 1, packet);
            if(!packet->referenceCount) enet_packet_destroy(packet);
            else { ++ws.uses; packet->freeCallback = cleanworldstate; }
        }
        c.messages.setsizenodelete(0);
    }
    reliablemessages = false;
    if(!ws.uses)
    {
        delete &ws;
        return false;
    }
    else
    {
        worldstates.add(&ws);
        return true;
    }
}

int maxclients = DEFAULTCLIENTS, kickthreshold = -5, banthreshold = -6;
string smapname, nextmapname, motd;
int nextgamemode;

const char *adminpasswd = NULL;

int countclients(int type, bool exclude = false)
{
    int num = 0;
    loopv(clients) if((clients[i]->type!=type)==exclude) num++;
    return num;
}

int numclients() { return countclients(ST_EMPTY, true); }
int numlocalclients() { return countclients(ST_LOCAL); }
int numnonlocalclients() { return countclients(ST_TCPIP); }

int numauthedclients()
{
    int num = 0;
    loopv(clients) if(clients[i]->type!=ST_EMPTY && clients[i]->isauthed) num++;
    return num;
}

int calcscores();

int freeteam(int pl = -1)
{
	int teamsize[2] = {0, 0};
	int teamscore[2] = {0, 0};
	int t;
    int sum = calcscores();
	loopv(clients) if(clients[i]->type!=ST_EMPTY && i != pl && clients[i]->isauthed)
	{
	    t = team_int(clients[i]->team);
	    teamsize[t]++;
        teamscore[t] += clients[i]->at3_score;
	}
	if(teamsize[0] == teamsize[1])
	{
        return sum > 200 ? (teamscore[0] < teamscore[1] ? 0 : 1) : rnd(2);
	}
	return teamsize[0] < teamsize[1] ? 0 : 1;
}

int findcnbyaddress(ENetAddress *address)
{
    loopv(clients)
    {
        if(clients[i]->type == ST_TCPIP && clients[i]->peer->address.host == address->host && clients[i]->peer->address.port == address->port)
            return i;
    }
    return -1;
}

savedscore *findscore(client &c, bool insert)
{
    if(c.type!=ST_TCPIP) return NULL;
    if(!insert)
    {
        loopv(clients)
        {
            client &o = *clients[i];
            if(o.type!=ST_TCPIP) continue;
            if(o.clientnum!=c.clientnum && o.peer->address.host==c.peer->address.host && !strcmp(o.name, c.name))
            {
                static savedscore curscore;
                curscore.save(o.state);
                return &curscore;
            }
        }
    }
    loopv(scores)
    {
        savedscore &sc = scores[i];
        if(!strcmp(sc.name, c.name) && sc.ip==c.peer->address.host) return &sc;
    }
    if(!insert) return NULL;
    savedscore &sc = scores.add();
    s_strcpy(sc.name, c.name);
    sc.ip = c.peer->address.host;
    return &sc;
}

struct server_entity            // server side version of "entity" type
{
    int type;
    bool spawned;
    int spawntime;
};

vector<server_entity> sents;

bool notgotitems = true;        // true when map has changed and waiting for clients to send item
int clnumspawn[3], clnumflagspawn[2];

// allows the gamemode macros to work with the server mode
#define gamemode smode
int smode = 0;

void restoreserverstate(vector<entity> &ents)   // hack: called from savegame code, only works in SP
{
    loopv(sents)
    {
        sents[i].spawned = ents[i].spawned;
        sents[i].spawntime = 0;
    }
}

static int interm = 0, minremain = 0, gamemillis = 0, gamelimit = 0;
static bool mapreload = false, autoteam = true, forceintermission = false;

static string serverpassword = "";
static string servdesc_full, servdesc_pre, servdesc_suf, servdesc_cur;
ENetAddress servdesc_caller;
bool custom_servdesc = false;

bool isdedicated;
ENetHost *serverhost = NULL;

void process(ENetPacket *packet, int sender, int chan);
void welcomepacket(ucharbuf &p, int n, ENetPacket *packet, bool forcedeath = false);
void sendwelcome(client *cl, int chan = 1, bool forcedeath = false);

void sendf(int cn, int chan, const char *format, ...)
{
    int exclude = -1;
    bool reliable = false;
    if(*format=='r') { reliable = true; ++format; }
    ENetPacket *packet = enet_packet_create(NULL, MAXTRANS, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    ucharbuf p(packet->data, packet->dataLength);
    va_list args;
    va_start(args, format);
    while(*format) switch(*format++)
    {
        case 'x':
            exclude = va_arg(args, int);
            break;

        case 'v':
        {
            int n = va_arg(args, int);
            int *v = va_arg(args, int *);
            loopi(n) putint(p, v[i]);
            break;
        }

        case 'i':
        {
            int n = isdigit(*format) ? *format++-'0' : 1;
            loopi(n) putint(p, va_arg(args, int));
            break;
        }
        case 's': sendstring(va_arg(args, const char *), p); break;
        case 'm':
        {
            int n = va_arg(args, int);
            enet_packet_resize(packet, packet->dataLength+n);
            p.buf = packet->data;
            p.maxlen += n;
            p.put(va_arg(args, uchar *), n);
            break;
        }
    }
    va_end(args);
    enet_packet_resize(packet, p.length());
    sendpacket(cn, chan, packet, exclude);
    if(packet->referenceCount==0) enet_packet_destroy(packet);
}

void sendservmsg(const char *msg, int client=-1)
{
    sendf(client, 1, "ris", SV_SERVMSG, msg);
}

void spawnstate(client *c)
{
    clientstate &gs = c->state;
    gs.spawnstate(smode);
    gs.lifesequence++;
}

void sendspawn(client *c)
{
    clientstate &gs = c->state;
    spawnstate(c);
    sendf(c->clientnum, 1, "ri7vv", SV_SPAWNSTATE, gs.lifesequence,
        gs.health, gs.armour,
        gs.primary, gs.gunselect, m_arena ? c->spawnindex : -1,
        NUMGUNS, gs.ammo, NUMGUNS, gs.mag);
    gs.lastspawn = gamemillis;
}

// demo

struct demofile
{
    string info;
    uchar *data;
    int len;
};

int maxdemos = 5;
vector<demofile> demos;

bool demonextmatch = false;
bool demoeverymatch = false;
FILE *demotmp = NULL;
gzFile demorecord = NULL, demoplayback = NULL;
bool recordpackets = false;
int nextplayback = 0;

void writedemo(int chan, void *data, int len)
{
    if(!demorecord) return;
    int stamp[3] = { gamemillis, chan, len };
    endianswap(stamp, sizeof(int), 3);
    gzwrite(demorecord, stamp, sizeof(stamp));
    gzwrite(demorecord, data, len);
}

void recordpacket(int chan, void *data, int len)
{
    if(recordpackets) writedemo(chan, data, len);
}

char *asctime()
{
    time_t t = time(NULL);
    char *timestr = ctime(&t);
    char *trim = timestr + strlen(timestr);
    while(trim>timestr && isspace(*--trim)) *trim = '\0';
    return timestr;
}

void enddemorecord()
{
    if(!demorecord) return;

    gzclose(demorecord);
    recordpackets = false;
    demorecord = NULL;

#ifdef WIN32
    demotmp = fopen(path("demos/demorecord", true), "rb");
#endif
    if(!demotmp) return;

    fseek(demotmp, 0, SEEK_END);
    int len = ftell(demotmp);
    rewind(demotmp);
    if(demos.length()>=maxdemos)
    {
        delete[] demos[0].data;
        demos.remove(0);
    }
    demofile &d = demos.add();
    s_sprintf(d.info)("%s: %s, %s, %.2f%s", asctime(), modestr(gamemode), smapname, len > 1024*1024 ? len/(1024*1024.f) : len/1024.0f, len > 1024*1024 ? "MB" : "kB");
    s_sprintfd(msg)("Demo \"%s\" recorded\nPress F10 to download it from the server..", d.info);
    sendservmsg(msg);
    logger->writeline(log::info, "Demo \"%s\" recorded.", d.info);
    d.data = new uchar[len];
    d.len = len;
    fread(d.data, 1, len, demotmp);
    fclose(demotmp);
    demotmp = NULL;
    if(demopath[0])
    {
        s_sprintf(msg)("%s%s_%s_%s.dmo", demopath, filenametime(), behindpath(smapname), modestr(gamemode, true));
        path(msg);
        FILE *demo = openfile(msg, "wb");
        if(demo)
        {
            int wlen = (int) fwrite(d.data, 1, d.len, demo);
            fclose(demo);
            logger->writeline(log::info, "demo written to file \"%s\" (%d bytes)", msg, wlen);
        }
        else
        {
            logger->writeline(log::info, "failed to write demo to file \"%s\"", msg);
        }
    }
}

void setupdemorecord()
{
    if(numlocalclients() || !m_mp(gamemode) || gamemode==1) return;

#ifdef WIN32
    gzFile f = gzopen(path("demos/demorecord", true), "wb9");
    if(!f) return;
#else
    demotmp = tmpfile();
    if(!demotmp) return;
    setvbuf(demotmp, NULL, _IONBF, 0);

    gzFile f = gzdopen(_dup(_fileno(demotmp)), "wb9");
    if(!f)
    {
        fclose(demotmp);
        demotmp = NULL;
        return;
    }
#endif

    sendservmsg("recording demo");
    logger->writeline(log::info, "Demo recording started.");

    demorecord = f;
    recordpackets = false;

    demoheader hdr;
    memcpy(hdr.magic, DEMO_MAGIC, sizeof(hdr.magic));
    hdr.version = DEMO_VERSION;
    hdr.protocol = PROTOCOL_VERSION;
    endianswap(&hdr.version, sizeof(int), 1);
    endianswap(&hdr.protocol, sizeof(int), 1);
    memset(hdr.desc, 0, DHDR_DESCCHARS);
    s_sprintfd(desc)("%s, %s, %s %s", modestr(gamemode, false), behindpath(smapname), asctime(), servdesc_cur);
    if(strlen(desc) > DHDR_DESCCHARS)
        s_sprintf(desc)("%s, %s, %s %s", modestr(gamemode, true), behindpath(smapname), asctime(), servdesc_cur);
    desc[DHDR_DESCCHARS - 1] = '\0';
    strcpy(hdr.desc, desc);
    gzwrite(demorecord, &hdr, sizeof(demoheader));

    ENetPacket *packet = enet_packet_create(NULL, MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
    ucharbuf p(packet->data, packet->dataLength);
    welcomepacket(p, -1, packet);
    writedemo(1, p.buf, p.len);
    enet_packet_destroy(packet);

    uchar buf[MAXTRANS];
    loopv(clients)
    {
        client *ci = clients[i];
        if(ci->type==ST_EMPTY) continue;

        uchar header[16];
        ucharbuf q(&buf[sizeof(header)], sizeof(buf)-sizeof(header));
        putint(q, SV_INITC2S);
        sendstring(ci->name, q);
        sendstring(ci->team, q);
        putint(q, ci->skin);

        ucharbuf h(header, sizeof(header));
        putint(h, SV_CLIENT);
        putint(h, ci->clientnum);
        putuint(h, q.len);

        memcpy(&buf[sizeof(header)-h.len], header, h.len);

        writedemo(1, &buf[sizeof(header)-h.len], h.len+q.len);
    }
}

void listdemos(int cn)
{
    ENetPacket *packet = enet_packet_create(NULL, MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
    if(!packet) return;
    ucharbuf p(packet->data, packet->dataLength);
    putint(p, SV_SENDDEMOLIST);
    putint(p, demos.length());
    loopv(demos) sendstring(demos[i].info, p);
    enet_packet_resize(packet, p.length());
    sendpacket(cn, 1, packet);
    if(!packet->referenceCount) enet_packet_destroy(packet);
}

static void cleardemos(int n)
{
    if(!n)
    {
        loopv(demos) delete[] demos[i].data;
        demos.setsize(0);
        sendservmsg("cleared all demos");
    }
    else if(demos.inrange(n-1))
    {
        delete[] demos[n-1].data;
        demos.remove(n-1);
        s_sprintfd(msg)("cleared demo %d", n);
        sendservmsg(msg);
    }
}

void senddemo(int cn, int num)
{
    if(!num) num = demos.length();
    if(!demos.inrange(num-1))
    {
        if(demos.empty()) sendservmsg("no demos available", cn);
        else
        {
            s_sprintfd(msg)("no demo %d available", num);
            sendservmsg(msg, cn);
        }
        return;
    }
    demofile &d = demos[num-1];
    sendf(cn, 2, "rim", SV_SENDDEMO, d.len, d.data);
}

void enddemoplayback()
{
    if(!demoplayback) return;
    gzclose(demoplayback);
    demoplayback = NULL;

    loopv(clients) sendf(i, 1, "ri3", SV_DEMOPLAYBACK, 0, i);

    sendservmsg("demo playback finished");

    loopv(clients) sendwelcome(clients[i]);
}

void setupdemoplayback()
{
    demoheader hdr;
    string msg;
    msg[0] = '\0';
    s_sprintfd(file)("demos/%s.dmo", smapname);
    path(file);
    demoplayback = opengzfile(file, "rb9");
    if(!demoplayback) s_sprintf(msg)("could not read demo \"%s\"", file);
    else if(gzread(demoplayback, &hdr, sizeof(demoheader))!=sizeof(demoheader) || memcmp(hdr.magic, DEMO_MAGIC, sizeof(hdr.magic)))
        s_sprintf(msg)("\"%s\" is not a demo file", file);
    else
    {
        endianswap(&hdr.version, sizeof(int), 1);
        endianswap(&hdr.protocol, sizeof(int), 1);
        if(hdr.version!=DEMO_VERSION) s_sprintf(msg)("demo \"%s\" requires an %s version of AssaultCube", file, hdr.version<DEMO_VERSION ? "older" : "newer");
        else if(hdr.protocol!=PROTOCOL_VERSION) s_sprintf(msg)("demo \"%s\" requires an %s version of AssaultCube", file, hdr.protocol<PROTOCOL_VERSION ? "older" : "newer");
    }
    if(msg[0])
    {
        if(demoplayback) { gzclose(demoplayback); demoplayback = NULL; }
        sendservmsg(msg);
        return;
    }

    s_sprintf(msg)("playing demo \"%s\"", file);
    sendservmsg(msg);

    sendf(-1, 1, "ri3", SV_DEMOPLAYBACK, 1, -1);

    if(gzread(demoplayback, &nextplayback, sizeof(nextplayback))!=sizeof(nextplayback))
    {
        enddemoplayback();
        return;
    }
    endianswap(&nextplayback, sizeof(nextplayback), 1);
}

void readdemo()
{
    if(!demoplayback) return;
    while(gamemillis>=nextplayback)
    {
        int chan, len;
        if(gzread(demoplayback, &chan, sizeof(chan))!=sizeof(chan) ||
           gzread(demoplayback, &len, sizeof(len))!=sizeof(len))
        {
            enddemoplayback();
            return;
        }
        endianswap(&chan, sizeof(chan), 1);
        endianswap(&len, sizeof(len), 1);
        ENetPacket *packet = enet_packet_create(NULL, len, 0);
        if(!packet || gzread(demoplayback, packet->data, len)!=len)
        {
            if(packet) enet_packet_destroy(packet);
            enddemoplayback();
            return;
        }
        sendpacket(-1, chan, packet);
        if(!packet->referenceCount) enet_packet_destroy(packet);
        if(gzread(demoplayback, &nextplayback, sizeof(nextplayback))!=sizeof(nextplayback))
        {
            enddemoplayback();
            return;
        }
        endianswap(&nextplayback, sizeof(nextplayback), 1);
    }
}

//

struct sflaginfo
{
    int state;
    int actor_cn;
    float pos[3];
    int lastupdate;
    int stolentime;
} sflaginfos[2];

void putflaginfo(ucharbuf &p, int flag)
{
    sflaginfo &f = sflaginfos[flag];
    putint(p, SV_FLAGINFO);
    putint(p, flag);
    putint(p, f.state);
    switch(f.state)
    {
        case CTFF_STOLEN:
            putint(p, f.actor_cn);
            break;
        case CTFF_DROPPED:
            loopi(3) putuint(p, uint(f.pos[i]*DMF));
            break;
    }
}

void sendflaginfo(int flag = -1, int cn = -1)
{
    ENetPacket *packet = enet_packet_create(NULL, MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
    ucharbuf p(packet->data, packet->dataLength);
    if(flag >= 0) putflaginfo(p, flag);
    else loopi(2) putflaginfo(p, i);
    enet_packet_resize(packet, p.length());
    sendpacket(cn, 1, packet);
    if(packet->referenceCount==0) enet_packet_destroy(packet);
}

void flagmessage(int flag, int message, int actor, int cn = -1)
{
    if(message == FM_KTFSCORE)
        sendf(cn, 1, "riiiii", SV_FLAGMSG, flag, message, actor, (gamemillis - sflaginfos[flag].stolentime) / 1000);
    else
        sendf(cn, 1, "riiii", SV_FLAGMSG, flag, message, actor);
}

void flagaction(int flag, int action, int actor)
{
    if(!valid_flag(flag)) return;
	sflaginfo &f = sflaginfos[flag];
	sflaginfo &of = sflaginfos[team_opposite(flag)];
	bool deadactor = valid_client(actor) ? clients[actor]->state.state != CS_ALIVE : true;
    int score = 0;
    int message = -1;

    if(m_ctf || m_htf)
    {
        switch(action)
        {
            case FA_PICKUP:  // ctf: f = enemy team    htf: f = own team
            {
                if(deadactor || f.state == CTFF_STOLEN) return;
                int team = team_int(clients[actor]->team);
                if(m_ctf) team = team_opposite(team);
                if(team != flag) return;
                f.state = CTFF_STOLEN;
                f.actor_cn = actor;
                message = FM_PICKUP;
                break;
            }
            case FA_LOST:
                if(actor == -1) actor = f.actor_cn;
            case FA_DROP:
                if(f.state!=CTFF_STOLEN || f.actor_cn != actor) return;
                f.state = CTFF_DROPPED;
                loopi(3) f.pos[i] = clients[actor]->state.o[i];
                message = action == FA_LOST ? FM_LOST : FM_DROP;
                break;
            case FA_RETURN:
                if(f.state!=CTFF_DROPPED || m_htf) return;
                f.state = CTFF_INBASE;
                message = FM_RETURN;
                break;
            case FA_SCORE:  // ctf: f = carried by actor flag,  htf: f = hunted flag (run over by actor)
                if(m_ctf)
                {
                    if(f.state != CTFF_STOLEN || f.actor_cn != actor || of.state != CTFF_INBASE) return;
                    score = 1;
                    message = FM_SCORE;
                }
                else // m_htf
                {
                    if(f.state != CTFF_DROPPED) return;
                    score = (of.state == CTFF_STOLEN) ? 1 : 0;
                    message = score ? FM_SCORE : FM_SCOREFAIL;
                    if(of.actor_cn == actor) score *= 2;
                }
                f.state = CTFF_INBASE;
                break;

            case FA_RESET:
                f.state = CTFF_INBASE;
                message = FM_RESET;
                break;
        }
    }
    else if(m_ktf)  // f: active flag, of: idle flag
    {
        switch(action)
        {
            case FA_PICKUP:
                if(deadactor || f.state != CTFF_INBASE) return;
                f.state = CTFF_STOLEN;
                f.actor_cn = actor;
                f.stolentime = gamemillis;
                message = FM_PICKUP;
                break;
            case FA_SCORE:  // f = carried by actor flag
                if(actor != -1 || f.state != CTFF_STOLEN) return; // no client msg allowed here
                if(valid_client(f.actor_cn) && clients[f.actor_cn]->state.state == CS_ALIVE)
                {
                    actor = f.actor_cn;
                    score = 1;
                    message = FM_KTFSCORE;
                    break;
                }
            case FA_LOST:
                if(actor == -1) actor = f.actor_cn;
            case FA_DROP:
                if(f.actor_cn != actor || f.state != CTFF_STOLEN) return;
            case FA_RESET:
                if(f.state == CTFF_STOLEN)
                {
                    actor = f.actor_cn;
                    message = FM_LOST;
                }
                f.state = CTFF_IDLE;
                of.state = CTFF_INBASE;
                sendflaginfo(team_opposite(flag));
                break;
        }
    }
    if(score)
    {
        clients[actor]->state.flagscore += score;
        sendf(-1, 1, "riii", SV_FLAGCNT, actor, clients[actor]->state.flagscore);
    }
    if(valid_client(actor))
    {
        client &c = *clients[actor];
        switch(message)
        {
            case FM_PICKUP:
                logger->writeline(log::info,"[%s] %s stole the flag", c.hostname, c.name);
                break;
            case FM_DROP:
            case FM_LOST:
                logger->writeline(log::info,"[%s] %s %s the flag", c.hostname, c.name, message == FM_LOST ? "lost" : "dropped");
                break;
            case FM_RETURN:
                logger->writeline(log::info,"[%s] %s returned the flag", c.hostname, c.name);
                break;
            case FM_SCORE:
                logger->writeline(log::info, "[%s] %s scored with the flag for %s, new score %d", c.hostname, c.name, c.team, c.state.flagscore);
                break;
            case FM_KTFSCORE:
                logger->writeline(log::info, "[%s] %s scored, carrying for %d seconds, new score %d", c.hostname, c.name, (gamemillis - f.stolentime) / 1000, c.state.flagscore);
                break;
            case FM_SCOREFAIL:
                logger->writeline(log::info, "[%s] %s failed to score", c.hostname, c.name);
                break;
            default:
                logger->writeline(log::info, "flagaction %d, actor %d, flag %d, message %d", action, actor, flag, message);
                break;
        }
    }
    else
    {
        switch(message)
        {
            case FM_RESET:
                logger->writeline(log::info,"the server reset the flag for team %s", team_string(flag));
                break;
            default:
                logger->writeline(log::info, "flagaction %d with invalid actor cn %d, flag %d, message %d", action, actor, flag, message);
                break;
        }
    }

	f.lastupdate = gamemillis;
	sendflaginfo(flag);
	if(message >= 0)
        flagmessage(flag, message, valid_client(actor) ? actor : -1);
}

void ctfreset()
{
    int idleflag = m_ktf ? rnd(2) : -1;
    loopi(2)
    {
        sflaginfos[i].actor_cn = -1;
        sflaginfos[i].state = i == idleflag ? CTFF_IDLE : CTFF_INBASE;
        sflaginfos[i].lastupdate = -1;
    }
}

void dropflag(int cn)
{
    if(m_flags && valid_client(cn))
    {
        loopi(2)
        {
            if(sflaginfos[i].state==CTFF_STOLEN && sflaginfos[i].actor_cn==cn)
                flagaction(i, FA_LOST, cn);
        }
    }
}

void resetflag(int cn)
{
    if(m_flags && valid_client(cn))
    {
        loopi(2)
        {
            if(sflaginfos[i].state==CTFF_STOLEN && sflaginfos[i].actor_cn==cn)
                flagaction(i, FA_RESET, -1);
        }
    }
}

void htf_forceflag(int flag)
{
    sflaginfo &f = sflaginfos[flag];
    int besthealth = 0, numbesthealth = 0;
    loopv(clients) if(clients[i]->type!=ST_EMPTY)
    {
        if(clients[i]->state.state == CS_ALIVE && team_int(clients[i]->team) == flag)
        {
            if(clients[i]->state.health == besthealth)
                numbesthealth++;
            else
            {
                if(clients[i]->state.health > besthealth)
                {
                    besthealth = clients[i]->state.health;
                    numbesthealth = 1;
                }
            }
        }
    }
    if(numbesthealth)
    {
        int pick = rnd(numbesthealth);
        loopv(clients) if(clients[i]->type!=ST_EMPTY)
        {
            if(clients[i]->state.state == CS_ALIVE && team_int(clients[i]->team) == flag && --pick < 0)
            {
                f.state = CTFF_STOLEN;
                f.actor_cn = i;
                sendflaginfo(flag);
                flagmessage(flag, FM_PICKUP, i);
                logger->writeline(log::info,"[%s] %s got forced to pickup the flag", clients[i]->hostname, clients[i]->name);
                break;
            }
        }
    }
    f.lastupdate = gamemillis;
}


bool canspawn(client *c, bool connecting = false)
{
    if(m_arena)
    {
        if(connecting && numauthedclients()<=2) return true;
        return false;
    }
    return true;
}

int arenaround = 0;

struct twoint { int index, value; };
int cmpscore(const void *a, const void * b) { return clients[*((int *)a)]->at3_score - clients[*((int *)b)]->at3_score; }
int cmptwoint(const void *a, const void * b) { return ((struct twoint *)a)->value - ((struct twoint *)b)->value; }
ivector tdistrib;
vector<twoint> sdistrib;

void distributeteam(int team)
{
    int numsp = team == 100 ? clnumspawn[2] : clnumspawn[team];
    if(!numsp) numsp = 30; // no map data yet: make a guess
    twoint ti;
    tdistrib.setsize(0);
    loopv(clients) if(clients[i]->type!=ST_EMPTY)
    {
        if(team == 100 || team == team_int(clients[i]->team))
        {
            tdistrib.add(i);
            clients[i]->at3_score = rand();
        }
    }
    tdistrib.sort(cmpscore); // random player order
    sdistrib.setsize(0);
    loopi(numsp)
    {
        ti.index = i;
        ti.value = rand();
        sdistrib.add(ti);
    }
    sdistrib.sort(cmptwoint); // random spawn order
    int x = 0;
    loopv(tdistrib)
    {
        clients[tdistrib[i]]->spawnindex = sdistrib[x++].index;
        x %= sdistrib.length();
    }
}

void distributespawns()
{
    loopv(clients) if(clients[i]->type!=ST_EMPTY)
    {
        clients[i]->spawnindex = -1;
    }
    if(m_teammode)
    {
        distributeteam(0);
        distributeteam(1);
    }
    else
    {
        distributeteam(100);
    }
}

void arenacheck()
{
    if(!m_arena || interm || gamemillis<arenaround || clients.empty()) return;

    if(arenaround)
    {   // start new arena round
        arenaround = 0;
        distributespawns();
        loopv(clients) if(clients[i]->type!=ST_EMPTY && clients[i]->isauthed)
        {
            clients[i]->state.respawn();
            sendspawn(clients[i]);
        }
        return;
    }

#ifndef STANDALONE
    if(m_botmode && clients[0]->type==ST_LOCAL)
    {
        bool alive = false, dead = false;
        loopv(players) if(players[i])
        {
            if(players[i]->state==CS_DEAD) dead = true;
            else alive = true;
        }
        if((dead && !alive) || player1->state==CS_DEAD)
        {
            sendf(-1, 1, "ri2", SV_ARENAWIN, player1->state==CS_ALIVE ? getclientnum() : (alive ? -2 : -1));
            arenaround = gamemillis+5000;
        }
        return;
    }
#endif
    client *alive = NULL;
    bool dead = false;
    int lastdeath = 0;
    loopv(clients)
    {
        client &c = *clients[i];
        if(c.type==ST_EMPTY || !c.isauthed) continue;
        if(c.state.state==CS_ALIVE || (c.state.state==CS_DEAD && c.state.lastspawn>=0))
        {
            if(!alive) alive = &c;
            else if(!m_teammode || strcmp(alive->team, c.team)) return;
        }
        else if(c.state.state==CS_DEAD)
        {
            dead = true;
            lastdeath = max(lastdeath, c.state.lastdeath);
        }
    }

    if(!dead || gamemillis < lastdeath + 500) return;
    sendf(-1, 1, "ri2", SV_ARENAWIN, alive ? alive->clientnum : -1);
    arenaround = gamemillis+5000;
    if(autoteam && m_teammode) refillteams(true);
}

#define SPAMREPEATINTERVAL  20   // detect doubled lines only if interval < 20 seconds
#define SPAMMAXREPEAT       3    // 4th time is SPAM
#define SPAMCHARPERMINUTE   220  // good typist
#define SPAMCHARINTERVAL    30   // allow 20 seconds typing at maxspeed

bool spamdetect(client *cl, char *text) // checks doubled lines and average typing speed
{
    if(cl->type != ST_TCPIP) return false;
    bool spam = false;
    int pause = servmillis - cl->lastsay;
    if(pause < 0 || pause > 90*1000) pause = 90*1000;
    cl->saychars -= (SPAMCHARPERMINUTE * pause) / (60*1000);
    cl->saychars += (int)strlen(text);
    if(cl->saychars < 0) cl->saychars = 0;
    if(text[0] && !strcmp(text, cl->lastsaytext) && servmillis - cl->lastsay < SPAMREPEATINTERVAL*1000)
    {
        spam = ++cl->spamcount > SPAMMAXREPEAT;
    }
    else
    {
         s_strcpy(cl->lastsaytext, text);
         cl->spamcount = 0;
    }
    cl->lastsay = servmillis;
    if(cl->saychars > (SPAMCHARPERMINUTE * SPAMCHARINTERVAL) / 60)
        spam = true;
    return spam;
}

void sendteamtext(char *text, int sender)
{
    if(!valid_client(sender) || !clients[sender]->team[0]) return;
    ENetPacket *packet = enet_packet_create(NULL, MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
    ucharbuf p(packet->data, packet->dataLength);
    putint(p, SV_TEAMTEXT);
    putint(p, sender);
    sendstring(text, p);
    enet_packet_resize(packet, p.length());
    loopv(clients) if(i!=sender)
    {
        if(!strcmp(clients[i]->team, clients[sender]->team) || !m_teammode) // send to everyone in non-team mode
            sendpacket(i, 1, packet);
    }
    if(packet->referenceCount==0) enet_packet_destroy(packet);
}

void sendvoicecomteam(int sound, int sender)
{
    if(!valid_client(sender) || !clients[sender]->team[0]) return;
    ENetPacket *packet = enet_packet_create(NULL, MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
    ucharbuf p(packet->data, packet->dataLength);
    putint(p, SV_VOICECOMTEAM);
    putint(p, sender);
    putint(p, sound);
    enet_packet_resize(packet, p.length());
    loopv(clients) if(i!=sender)
    {
        if(!strcmp(clients[i]->team, clients[sender]->team) || !m_teammode)
            sendpacket(i, 1, packet);
    }
    if(packet->referenceCount==0) enet_packet_destroy(packet);
}

void resetitems() { sents.setsize(0); notgotitems = true; }

int spawntime(int type)
{
    int np = numclients();
    np = np<3 ? 4 : (np>4 ? 2 : 3);         // spawn times are dependent on number of players
    int sec = 0;
    switch(type)
    {
        case I_CLIPS:
        case I_AMMO:
        case I_GRENADE: sec = np*2; break;
        case I_HEALTH: sec = np*5; break;
        case I_ARMOUR: sec = 20; break;
        case I_AKIMBO: sec = 60; break;
    }
    return sec*1000;
}

bool serverpickup(int i, int sender)         // server side item pickup, acknowledge first client that gets it
{
    if(!sents.inrange(i)) return false;
    server_entity &e = sents[i];
    if(!e.spawned) return false;
    if(sender>=0)
    {
        client *cl = clients[sender];
        if(cl->type==ST_TCPIP)
        {
            if(cl->state.state!=CS_ALIVE || !cl->state.canpickup(e.type)) return false;
        }
        sendf(-1, 1, "ri3", SV_ITEMACC, i, sender);
        cl->state.pickup(sents[i].type);
    }
    e.spawned = false;
    e.spawntime = spawntime(e.type);
    return true;
}

void checkitemspawns(int diff)
{
    if(!diff) return;
    loopv(sents) if(sents[i].spawntime)
    {
        sents[i].spawntime -= diff;
        if(sents[i].spawntime<=0)
        {
            sents[i].spawntime = 0;
            sents[i].spawned = true;
            sendf(-1, 1, "ri2", SV_ITEMSPAWN, i);
        }
    }
}

void serverdamage(client *target, client *actor, int damage, int gun, bool gib, const vec &hitpush = vec(0, 0, 0))
{
    clientstate &ts = target->state;
    ts.dodamage(damage);
    actor->state.damage += damage != 1000 ? damage : 0;
    sendf(-1, 1, "ri6", gib ? SV_GIBDAMAGE : SV_DAMAGE, target->clientnum, actor->clientnum, damage, ts.armour, ts.health);
    if(target!=actor && !hitpush.iszero())
    {
        vec v(hitpush);
        if(!v.iszero()) v.normalize();
        sendf(target->clientnum, 1, "ri6", SV_HITPUSH, gun, damage,
            int(v.x*DNF), int(v.y*DNF), int(v.z*DNF));
    }
    if(ts.health<=0)
    {
        int targethasflag = -1;
        bool tk = false, suic = false;
        loopi(2) { if(sflaginfos[i].state == CTFF_STOLEN && sflaginfos[i].actor_cn == target->clientnum) targethasflag = i; }
        target->state.deaths++;
        if(target!=actor)
        {
            if(!isteam(target->team, actor->team)) actor->state.frags += gib ? 2 : 1;
            else
            {
                actor->state.frags--;
                actor->state.teamkills++;
                tk = true;
            }
        }
        else
        { // suicide
            actor->state.frags--;
            suic = true;
            logger->writeline(log::info, "[%s] %s suicided", actor->hostname, actor->name);
        }
        sendf(-1, 1, "ri4", gib ? SV_GIBDIED : SV_DIED, target->clientnum, actor->clientnum, actor->state.frags);
        if((suic || tk) && (m_htf || m_ktf) && targethasflag >= 0)
        {
            actor->state.flagscore--;
            sendf(-1, 1, "riii", SV_FLAGCNT, actor->clientnum, actor->state.flagscore);
        }
        target->position.setsizenodelete(0);
        ts.state = CS_DEAD;
        ts.lastdeath = gamemillis;
        if(!suic) logger->writeline(log::info, "[%s] %s %s%s %s", actor->hostname, actor->name, gib ? "gibbed" : "fragged", tk ? " his teammate" : "", target->name);
        if(m_flags && targethasflag >= 0)
        {
            if(m_ctf)
                flagaction(targethasflag, tk ? FA_RESET : FA_LOST, -1);
            else if(m_htf)
                flagaction(targethasflag, FA_LOST, -1);
            else // ktf || tktf
                flagaction(targethasflag, FA_RESET, -1);
        }
        // don't issue respawn yet until DEATHMILLIS has elapsed
        // ts.respawn();

        if(actor->state.frags < kickthreshold) disconnect_client(actor->clientnum, DISC_AUTOKICK);
        else if(actor->state.frags < banthreshold)
        {
            ban b = { actor->peer->address, servmillis+20*60*1000 };
		    bans.add(b);
            disconnect_client(actor->clientnum, DISC_AUTOBAN);
        }
    }
}

#include "serverevents.h"

struct configset
{
    string mapname;
    int mode;
    int time;
    bool vote;
    int minplayer;
    int maxplayer;
    int skiplines;
};

vector<configset> configsets;
int curcfgset = -1;

char *loadcfgfile(char *cfg, const char *name, int *len)
{
    if(name && name[0])
    {
        s_strcpy(cfg, name);
        path(cfg);
    }
    char *buf = loadfile(cfg, len);
    if(!buf)
    {
        if(name) logger->writeline(log::info,"could not read config file '%s'", name);
        return NULL;
    }
    char *p = buf;
    while((p = strstr(p, "//")) != NULL) // remove comments
        while(p[0] != '\n' && p[0] != '\0') p++[0] = ' ';
    if('\r' != '\n') // this is not a joke!
    {
        p = buf;
        while((p = strchr(p, '\r')) != NULL) p++[0] = ' ';
    }
    p = buf;
    while((p = strchr(p, '\n')) != NULL) p++[0] = 0;
    return buf;
}

#define CONFIG_MAXPAR 6

extern const char *fullmodestr(int n);

void readscfg(const char *name)
{
    static string cfgfilename;
    static int cfgfilesize;
    const char *sep = ": ";
    configset c;
    char *p, *l;
    int i, len, par[CONFIG_MAXPAR];

    if(!name && getfilesize(cfgfilename) == cfgfilesize) return;
    configsets.setsize(0);
    char *buf = loadcfgfile(cfgfilename, name, &len);
    cfgfilesize = len;
    if(!buf) return;
    p = buf;
    if(verbose) logger->writeline(log::info,"reading map rotation '%s'", cfgfilename);
    while(p < buf + len)
    {
        l = p; p += strlen(p) + 1;
        l = strtok(l, sep);
        if(l)
        {
            s_strcpy(c.mapname, l);
            par[3] = par[4] = par[5] = 0;  // default values
            for(i = 0; i < CONFIG_MAXPAR; i++)
            {
                if((l = strtok(NULL, sep)) != NULL)
                    par[i] = atoi(l);
                else
                    break;
            }
            if(i > 2)
            {
                c.mode = par[0];
                c.time = par[1];
                c.vote = par[2] > 0;
                c.minplayer = par[3];
                c.maxplayer = par[4];
                c.skiplines = par[5];
                configsets.add(c);
                if(verbose) logger->writeline(log::info," %s, %s, %d minutes, vote:%d, minplayer:%d, maxplayer:%d, skiplines:%d", c.mapname, fullmodestr(c.mode), c.time, c.vote, c.minplayer, c.maxplayer, c.skiplines);
            }
        }
    }
    delete[] buf;
}

struct iprange { enet_uint32 lr, ur; };
int cmpiprange(const void *a, const void * b) { return ((struct iprange *)a)->lr - ((struct iprange *)b)->lr; }
int cmpipmatch(const void *a, const void * b) { return - (((struct iprange *)a)->lr < ((struct iprange *)b)->lr) + (((struct iprange *)a)->lr > ((struct iprange *)b)->ur); }

enet_uint32 atoip(const char *s)
{
    unsigned int d[3], res;
    if(sscanf(s, "%u.%u.%u.%u", &res, d, d + 1, d + 2) != 4) return 0;
    loopi(3)
    {
        if(d[i] > 255) return 0;
        res = (res << 8) + d[i];
    }
    return res;
}

const char *iptoa(enet_uint32 ip, int buf = 0)
{
    static string s[4];
    s_sprintf(s[buf & 3])("%d.%d.%d.%d", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return s[buf & 3];
}

vector<iprange> blacklist;

void readblacklist(const char *name)
{
    static string blfilename;
    static int blfilesize;
    char *p, *l;
    iprange ir;
    int m, len;

    if(!name && getfilesize(blfilename) == blfilesize) return;
    blacklist.setsize(0);
    char *buf = loadcfgfile(blfilename, name, &len);
    blfilesize = len;
    if(!buf) return;
    p = buf;
    if(verbose) logger->writeline(log::info,"reading blacklist '%s'", blfilename);
    while(p < buf + len)
    {
        l = p; p += strlen(p) + 1;
        if(!(ir.lr = ir.ur = atoip(l))) continue;
        if(strchr(l, '-'))
        {
            ir.ur = atoip(strchr(l, '-') + 1);
            if(!ir.ur || ir.lr > ir.ur) continue;
        }
        else if(strchr(l, '/'))
        {
            m = atoi(strchr(l, '/') + 1);
            if(m > 0 && m < 33)
            {
                m = (1 << (32 - m)) - 1;
                ir.lr &= ~m;
                ir.ur |= m;
            }
            else continue;
        }
        blacklist.add(ir);
    }
    delete[] buf;
    blacklist.sort(cmpiprange);
    int orglength = blacklist.length();
    loopv(blacklist)
    {
        if(!i) continue;
        if(blacklist[i].ur <= blacklist[i - 1].ur)
        {
            if(verbose) logger->writeline(log::info," blacklist entry %s-%s got dropped (range already covered by %s-%s)",
                iptoa(blacklist[i].lr, 0), iptoa(blacklist[i].ur, 1), iptoa(blacklist[i - 1].lr, 2), iptoa(blacklist[i - 1].ur, 3));
            blacklist.remove(i--); continue;
        }
        if(blacklist[i].lr <= blacklist[i - 1].ur)
        {
            if(verbose) logger->writeline(log::info," blacklist entries %s-%s and %s-%s are joined due to overlap",
                iptoa(blacklist[i - 1].lr, 0), iptoa(blacklist[i - 1].ur, 1), iptoa(blacklist[i].lr, 2), iptoa(blacklist[i].ur, 3));
            blacklist[i - 1].ur = blacklist[i].ur;
            blacklist.remove(i--); continue;
        }
    }
    if(verbose)
    {
        loopv(blacklist)
        {
            if(blacklist[i].lr == blacklist[i].ur)
                logger->writeline(log::info," %s", iptoa(blacklist[i].lr, 0));
            else
                logger->writeline(log::info," %s-%s", iptoa(blacklist[i].lr, 0), iptoa(blacklist[i].ur, 1));
        }
    }
    logger->writeline(log::info,"read %d (%d) blacklist entries from %s", blacklist.length(), orglength, blfilename);
}

bool checkblacklist(enet_uint32 ip)
{
    iprange t;
    t.lr = ip;
    t.ur = 0;
    return blacklist.search(&t, cmpipmatch) != NULL;
}

struct pwddetail
{
    string pwd;
    int line;
    bool denyadmin;    // true: connect only
};

vector<pwddetail> adminpwds;
#define ADMINPWD_MAXPAR 1

void readpwdfile(const char *name)
{
    static string pwdfilename;
    static int pwdfilesize;
    const char *sep = " ";
    pwddetail c;
    char *p, *l;
    int i, len, line, par[ADMINPWD_MAXPAR];

    if(!name && getfilesize(pwdfilename) == pwdfilesize) return;
    adminpwds.setsize(0);
    if(adminpasswd && adminpasswd[0])
    {
        s_strcpy(c.pwd, adminpasswd);
        c.line = 0;   // commandline is 'line 0'
        c.denyadmin = false;
        adminpwds.add(c);
    }
    char *buf = loadcfgfile(pwdfilename, name, &len);
    pwdfilesize = len;
    if(!buf) return;
    p = buf; line = 1;
    if(verbose) logger->writeline(log::info,"reading admin passwords '%s'", pwdfilename);
    while(p < buf + len)
    {
        l = p; p += strlen(p) + 1;
        l = strtok(l, sep);
        if(l)
        {
            s_strcpy(c.pwd, l);
            par[0] = 0;  // default values
            for(i = 0; i < ADMINPWD_MAXPAR; i++)
            {
                if((l = strtok(NULL, sep)) != NULL)
                    par[i] = atoi(l);
                else
                    break;
            }
            //if(i > 0)
            {
                c.line = line;
                c.denyadmin = par[0] > 0;
                adminpwds.add(c);
                if(verbose) logger->writeline(log::info,"line%4d: %s %d", c.line, c.pwd, c.denyadmin ? 1 : 0);
            }
        }
        line++;
    }
    delete[] buf;
    logger->writeline(log::info,"read %d admin passwords from %s", adminpwds.length() - (adminpasswd && adminpasswd[0]), name ? name : "");
}

bool checkadmin(const char *name, const char *pwd, int salt, pwddetail *detail = NULL)
{
    bool found = false;
    loopv(adminpwds)
    {
        if(!strcmp(genpwdhash(name, adminpwds[i].pwd, salt), pwd))
        {
            if(detail) *detail = adminpwds[i];
            found = true;
            break;
        }
    }
    return found;
}

bool updatedescallowed(void) { return servdesc_pre[0] || servdesc_suf[0]; }

void updatesdesc(const char *newdesc, ENetAddress *caller = NULL)
{
    if(!newdesc || !newdesc[0] || !updatedescallowed())
    {
        s_strcpy(servdesc_cur, servdesc_full);
        custom_servdesc = false;
    }
    else
    {
        s_sprintf(servdesc_cur)("%s%s%s", servdesc_pre, newdesc, servdesc_suf);
        custom_servdesc = true;
        if(caller) servdesc_caller = *caller;
    }
    servermsdesc(servdesc_cur);
}

void resetvotes(int result)
{
    loopv(clients)
	{
		clients[i]->vote = VOTE_NEUTRAL;
		if(result == VOTE_YES) clients[i]->lastvotecall = 0; // flowtron: successful votes and mapchange reset the timer
	}
}

void forceteam(int client, int team, bool respawn, bool notify = false)
{
    if(!valid_client(client) || team < 0 || team > 1) return;
    if(clients[client]->lastforce && (servmillis - clients[client]->lastforce) < 2000) return;
    sendf(client, 1, "riii", SV_FORCETEAM, team, (respawn ? 1 : 0) | (respawn && !notify ? 2 : 0));
    clients[client]->lastforce = servmillis;
    if(notify) sendf(-1, 1, "riii", SV_FORCENOTIFY, client, team);
}

int calcscores() // skill eval
{
    int fp12 = (m_ctf || m_htf) ? 55 : 33;
    int fp3 = (m_ctf || m_htf) ? 25 : 15;
    int sum = 0;
    loopv(clients) if(clients[i]->type!=ST_EMPTY)
    {
        clientstate &cs = clients[i]->state;
        sum += clients[i]->at3_score = (cs.frags * 100) / (cs.deaths ? cs.deaths : 1)
                                     + (cs.flagscore < 3 ? fp12 * cs.flagscore : 2 * fp12 + fp3 * (cs.flagscore - 2));
    }
    return sum;
}

ivector shuffle;

void shuffleteams(bool respawn = true)
{
    int numplayers = numclients();
    int team, sums = calcscores();
    if(gamemillis < 2 * 60 *1000)
    { // random
        int teamsize[2] = {0, 0};
        loopv(clients) if(clients[i]->type!=ST_EMPTY)
        {
            sums += rnd(1000);
            team = sums & 1;
            if(teamsize[team] >= numplayers/2) team = team_opposite(team);
            forceteam(i, team, respawn);
            teamsize[team]++;
            sums >>= 1;
        }
    }
    else
    { // skill sorted
        shuffle.setsize(0);
        sums /= 4 * numplayers + 2;
        team = rnd(2);
        loopv(clients) if(clients[i]->type!=ST_EMPTY) { clients[i]->at3_score += rnd(sums); shuffle.add(i); }
        shuffle.sort(cmpscore);
        loopi(shuffle.length())
        {
            forceteam(shuffle[i], team, respawn);
            team = !team;
        }
    }
}

bool refillteams(bool now, bool notify)  // force only minimal amounts of players
{
    static int lasttime_eventeams = 0;
    int teamsize[2] = {0, 0}, teamscore[2] = {0, 0}, moveable[2] = {0, 0};
    bool switched = false;

    calcscores();
    loopv(clients) if(clients[i]->type!=ST_EMPTY)     // playerlist stocktaking
    {
        client *c = clients[i];
        c->at3_dontmove = true;
        if(c->isauthed)
        {
            int t = 0;
            if(!strcmp(c->team, "CLA") || t++ || !strcmp(c->team, "RVSF")) // need exact teams here
            {
                teamsize[t]++;
                teamscore[t] += c->at3_score;
                if(!m_flags || !((sflaginfos[0].state==CTFF_STOLEN && sflaginfos[0].actor_cn==i) ||
                                 (sflaginfos[1].state==CTFF_STOLEN && sflaginfos[1].actor_cn==i)   ))
                {
                    c->at3_dontmove = false;
                    moveable[t]++;
                    if(c->lastforce && (servmillis - c->lastforce) < 3000) return false; // possible unanswered forceteam commands
                }
            }
        }
    }
    int bigteam = teamsize[1] > teamsize[0];
    int allplayers = teamsize[0] + teamsize[1];
    int diffnum = teamsize[bigteam] - teamsize[!bigteam];
    int diffscore = teamscore[bigteam] - teamscore[!bigteam];
    if(lasttime_eventeams > gamemillis) lasttime_eventeams = 0;
    if(diffnum > 1)
    {
        if(now || gamemillis - lasttime_eventeams > 8000 + allplayers * 1000 || diffnum > 2 + allplayers / 10)
        {
            // time to even out teams
            loopv(clients) if(clients[i]->type!=ST_EMPTY && team_int(clients[i]->team) != bigteam) clients[i]->at3_dontmove = true;  // dont move small team players
            while(diffnum > 1 && moveable[bigteam] > 0)
            {
                // pick best fitting cn
                string atlog, buf;    // debug logging - will be removed
                int pick = -1;
                int bestfit = 1000000000;
                int targetscore = diffscore / (diffnum & ~1);
                s_sprintf(atlog)("at-target: %d, ", targetscore);
                loopv(clients) if(clients[i]->type!=ST_EMPTY && !clients[i]->at3_dontmove) // try all still movable players
                {
                    int fit = targetscore - clients[i]->at3_score;
                    if(fit < 0 ) fit = -(fit * 15) / 10;       // avoid too good players
                    int forcedelay = clients[i]->at3_lastforce ? (1000 - (gamemillis - clients[i]->at3_lastforce) / (5 * 60)) : 0;
                    if(forcedelay > 0) fit += (fit * forcedelay) / 600;   // avoid lately forced players
                    if(fit < bestfit + fit * rnd(100) / 400)   // search 'almost' best fit
                    {
                        bestfit = fit;
                        pick = i;
                    }
                    s_sprintf(buf)("%d:%d ", i, fit); s_strcat(atlog, buf);
                }
                if(pick < 0) break; // should really never happen
                // move picked player
                forceteam(pick, !bigteam, true, notify);

                diffnum -= 2;
                diffscore -= 2 * clients[pick]->at3_score;
                moveable[bigteam]--;
                clients[pick]->at3_dontmove = true;
                clients[pick]->at3_lastforce = gamemillis;  // try not to force this player again for the next 5 minutes
                switched = true;
                s_sprintf(buf)(" pick:%d", pick); s_strcat(atlog, buf);
                logger->writeline(log::info,"%s", atlog);
            }
        }
    }
    if(diffnum < 2)
        lasttime_eventeams = gamemillis;
    return switched;
}

bool mapavailable(const char *mapname);
void getservermap(void);

void resetmap(const char *newname, int newmode, int newtime, bool notify)
{
    if(m_demo) enddemoplayback();
    else enddemorecord();

    if(custom_servdesc && findcnbyaddress(&servdesc_caller) < 0)
    {
        updatesdesc(NULL);
        if(notify)
        {
            sendservmsg("server description reset to default");
            logger->writeline(log::info, "server description reset to '%s'", servdesc_cur);
        }
    }

	bool lastteammode = m_teammode;
    smode = newmode;
    s_strcpy(smapname, newname);
    if(isdedicated && smapname[0]) getservermap();

    minremain = newtime >= 0 ? newtime : (m_teammode ? 15 : 10);
    gamemillis = 0;
    gamelimit = minremain*60000;

    mapreload = false;
    interm = 0;
    if(!laststatus) laststatus = servmillis-61*1000;
    lastfillup = servmillis;
    resetvotes(VOTE_YES); // flowtron: VOTE_YES => reset lastvotecall too
    resetitems();
    loopi(3) clnumspawn[i] = 0;
    loopi(2) clnumflagspawn[i] = 0;
    scores.setsize(0);
    ctfreset();
    if(notify)
    {
        // change map
        sendf(-1, 1, "risii", SV_MAPCHANGE, smapname, smode, mapavailable(smapname) ? 1 : 0);
        if(smode>1 || (smode==0 && numnonlocalclients()>0)) sendf(-1, 1, "ri2", SV_TIMEUP, minremain);
        logger->writeline(log::info, "\nGame start: %s on %s, %d players, %d minutes remaining, mastermode %d", modestr(smode), smapname, numclients(), minremain, mastermode);
    }
    if(m_arena)
    {
        arenaround = 0;
        distributespawns();
    }
    if(notify)
    {
        // shuffle if previous mode wasn't a team-mode
        if(m_teammode)
        {
            if(!lastteammode)
                shuffleteams(false);
            else if(autoteam)
                refillteams(true, false);
        }
        // send spawns
        loopv(clients) if(clients[i]->type!=ST_EMPTY)
        {
            client *c = clients[i];
            c->mapchange();
            if(m_mp(smode)) sendspawn(c);
        }
    }
    if(m_demo) setupdemoplayback();
    else if((demonextmatch || demoeverymatch) && *newname && numnonlocalclients() > 0)
    {
        demonextmatch = false;
        setupdemorecord();
    }
    if(notify && m_ktf) sendflaginfo();

    nextmapname[0] = '\0';
    forceintermission = false;
}

int nextcfgset(bool notify = true, bool nochange = false) // load next maprotation set
{
    int n = numclients();
    int csl = configsets.length();
    int ccs = curcfgset;
    if(ccs >= 0 && ccs < csl) ccs += configsets[ccs].skiplines;
    configset *c = NULL;
    loopi(csl)
    {
        ccs++;
        if(ccs >= csl || ccs < 0) ccs = 0;
        c = &configsets[ccs];
        if(n >= c->minplayer && (!c->maxplayer || n <= c->maxplayer)) break;
    }
    if(!nochange)
    {
        curcfgset = ccs;
        resetmap(c->mapname, c->mode, c->time, notify);
    }
    return ccs;
}

bool isbanned(int cn)
{
	if(!valid_client(cn)) return false;
	client &c = *clients[cn];
	loopv(bans)
	{
		ban &b = bans[i];
		if(b.millis < servmillis) { bans.remove(i--); }
		if(b.address.host == c.peer->address.host) { return true; }
	}
	return checkblacklist(atoip(c.hostname));
}

int serveroperator()
{
	loopv(clients) if(clients[i]->type!=ST_EMPTY && clients[i]->role > CR_DEFAULT) return i;
	return -1;
}

void sendserveropinfo(int receiver)
{
    int op = serveroperator();
    sendf(receiver, 1, "riii", SV_SERVOPINFO, op, op >= 0 ? clients[op]->role : -1);
}

#include "serveractions.h"

struct voteinfo
{
    int owner, callmillis, result;
    serveraction *action;

    voteinfo() : owner(0), callmillis(0), result(VOTE_NEUTRAL), action(NULL) {}

    void end(int result)
    {
        resetvotes(result);
        sendf(-1, 1, "ri2", SV_VOTERESULT, result);
        if(result == VOTE_YES && action)
        {
            //if(demorecord) enddemorecord();
            action->perform();
        }
        this->result = result;
    }

    bool isvalid() { return valid_client(owner) && action != NULL && action->isvalid(); }
    bool isalive() { return servmillis < callmillis+40*1000; }

    void evaluate(bool forceend = false)
    {
        if(result!=VOTE_NEUTRAL) return; // block double action
        if(action && !action->isvalid()) end(VOTE_NO);
        int stats[VOTE_NUM] = {0};
        int adminvote = VOTE_NEUTRAL;
        loopv(clients)
            if(clients[i]->type!=ST_EMPTY && clients[i]->connectmillis < callmillis)
            {
                stats[clients[i]->vote]++;
                if(clients[i]->role==CR_ADMIN) adminvote = clients[i]->vote;
            };

        bool admin = clients[owner]->role==CR_ADMIN || (!isdedicated && clients[owner]->type==ST_LOCAL);
        int total = stats[VOTE_NO]+stats[VOTE_YES]+stats[VOTE_NEUTRAL];
        const float requiredcount = 0.51f;
        if(stats[VOTE_YES]/(float)total > requiredcount || admin || adminvote == VOTE_YES)
            end(VOTE_YES);
        else if(forceend || stats[VOTE_NO]/(float)total > requiredcount || stats[VOTE_NO] >= stats[VOTE_YES]+stats[VOTE_NEUTRAL] || adminvote == VOTE_NO)
            end(VOTE_NO);
        else return;
    }
};

static voteinfo *curvote = NULL;

bool vote(int sender, int vote, ENetPacket *msg) // true if the vote was placed successfully
{
    if(!curvote || !valid_client(sender) || vote < VOTE_YES || vote > VOTE_NO) return false;
    if(clients[sender]->vote != VOTE_NEUTRAL)
    {
        sendf(sender, 1, "ri2", SV_CALLVOTEERR, VOTEE_MUL);
        return false;
    }
    else
    {
        sendpacket(-1, 1, msg, sender);

        clients[sender]->vote = vote;
        curvote->evaluate();
        return true;
    }
}

void callvotesuc(voteinfo *v)
{
    if(!v->isvalid()) return;
    DELETEP(curvote);
    curvote = v;
    clients[v->owner]->lastvotecall = servmillis;

    sendf(v->owner, 1, "ri", SV_CALLVOTESUC);
    logger->writeline(log::info, "[%s] client %s called a vote: %s", clients[v->owner]->hostname, clients[v->owner]->name, v->action->desc ? v->action->desc : "[unknown]");
}

void callvoteerr(voteinfo *v, int error)
{
    if(!valid_client(v->owner)) return;
    sendf(v->owner, 1, "ri2", SV_CALLVOTEERR, error);
    logger->writeline(log::info, "[%s] client %s failed to call a vote: %s (%s)", clients[v->owner]->hostname, clients[v->owner]->name, v->action->desc ? v->action->desc : "[unknown]", voteerrorstr(error));
}

bool callvote(voteinfo *v, ENetPacket *msg) // true if a regular vote was called
{
    int area = isdedicated ? EE_DED_SERV : EE_LOCAL_SERV;
    int error = -1;

    if(!v || !v->isvalid()) error = VOTEE_INVALID;
    else if(v->action->role > clients[v->owner]->role) error = VOTEE_PERMISSION;
    else if(!(area & v->action->area)) error = VOTEE_AREA;
    else if(curvote && curvote->result==VOTE_NEUTRAL) error = VOTEE_CUR;
    else if(clients[v->owner]->role == CR_DEFAULT && v->action->isdisabled()) error = VOTEE_DISABLED;
    else if(clients[v->owner]->lastvotecall && servmillis - clients[v->owner]->lastvotecall < 60*1000 && clients[v->owner]->role != CR_ADMIN && numclients()>1)
        error = VOTEE_MAX;

    if(error>=0)
    {
        callvoteerr(v, error);
        return false;
    }
    else
    {
        sendpacket(-1, 1, msg, v->owner);

        callvotesuc(v);
        return true;
    }
}

void changeclientrole(int client, int role, char *pwd, bool force)
{
    pwddetail pd;
    if(!isdedicated || !valid_client(client)) return;
    pd.line = -1;
    if(force || role == CR_DEFAULT || (role == CR_ADMIN && pwd && pwd[0] && checkadmin(clients[client]->name, pwd, clients[client]->salt, &pd) && !pd.denyadmin))
    {
        if(role == clients[client]->role) return;
        if(role > CR_DEFAULT)
        {
            loopv(clients) clients[i]->role = CR_DEFAULT;
        }
        clients[client]->role = role;
        sendserveropinfo(-1);
        if(pd.line > -1)
            logger->writeline(log::info,"[%s] player %s used admin password in line %d", clients[client]->hostname, clients[client]->name[0] ? clients[client]->name : "[unnamed]", pd.line);
        logger->writeline(log::info,"[%s] set role of player %s to %s", clients[client]->hostname, clients[client]->name[0] ? clients[client]->name : "[unnamed]", role == CR_ADMIN ? "admin" : "normal player"); // flowtron : connecting players haven't got a name yet (connectadmin)
    }
    else if(pwd && pwd[0]) disconnect_client(client, DISC_SOPLOGINFAIL); // avoid brute-force
    if(curvote) curvote->evaluate();
}

const char *disc_reason(int reason)
{
    static const char *disc_reasons[] = { "normal", "end of packet", "client num", "kicked by server operator", "banned by server operator", "tag type", "connection refused due to ban", "wrong password", "failed admin login", "server FULL - maxclients", "server mastermode is \"private\"", "auto kick - did your score drop below the threshold?", "auto ban - did your score drop below the threshold?", "duplicate connection" };
    return reason >= 0 && (size_t)reason < sizeof(disc_reasons)/sizeof(disc_reasons[0]) ? disc_reasons[reason] : "unknown";
}

void disconnect_client(int n, int reason)
{
    if(!clients.inrange(n) || clients[n]->type!=ST_TCPIP) return;
    dropflag(n);
    client &c = *clients[n];
    savedscore *sc = findscore(c, true);
    if(sc) sc->save(c.state);
    if(reason>=0) logger->writeline(log::info, "[%s] disconnecting client %s (%s)", c.hostname, c.name, disc_reason(reason));
    else logger->writeline(log::info, "[%s] disconnected client %s", c.hostname, c.name);
    c.peer->data = (void *)-1;
    if(reason>=0) enet_peer_disconnect(c.peer, reason);
	clients[n]->zap();
    sendf(-1, 1, "rii", SV_CDIS, n);
    if(curvote) curvote->evaluate();
}

void sendwhois(int sender, int cn)
{
    if(!valid_client(sender) || !valid_client(cn)) return;
    if(clients[cn]->type == ST_TCPIP && clients[cn]->peer)
    {
        uint ip = clients[cn]->peer->address.host;
        if(clients[sender]->role != CR_ADMIN) ip &= 0xFFFF; // only admin gets full IP
        sendf(sender, 1, "ri3", SV_WHOISINFO, cn, ip);
    }
}

// sending of maps between clients

string copyname;
int copysize, copymapsize, copycfgsize, copycfgsizegz;
uchar *copydata = NULL;

bool mapavailable(const char *mapname) { return !strcmp(copyname, mapname); }

bool sendmapserv(int n, string mapname, int mapsize, int cfgsize, int cfgsizegz, uchar *data)
{
    string name;
    FILE *fp;
    bool written = false;

    if(!mapname[0] || mapsize <= 0 || mapsize + cfgsizegz > MAXMAPSENDSIZE || cfgsize > MAXCFGFILESIZE) return false;
    s_strcpy(copyname, mapname);
    copymapsize = mapsize;
    copycfgsize = cfgsize;
    copycfgsizegz = cfgsizegz;
    copysize = mapsize + cfgsizegz;
    DELETEA(copydata);
    copydata = new uchar[copysize];
    memcpy(copydata, data, copysize);

    s_sprintf(name)(SERVERMAP_PATH_INCOMING "%s.cgz", behindpath(copyname));
    path(name);
    fp = fopen(name, "wb");
    if(fp)
    {
        fwrite(copydata, 1, copymapsize, fp);
        fclose(fp);
        s_sprintf(name)(SERVERMAP_PATH_INCOMING "%s.cfg", behindpath(copyname));
        path(name);
        fp = fopen(name, "wb");
        if(fp)
        {
            uchar *rawcfg = new uchar[copycfgsize];
            uLongf rawsize = copycfgsize;
            if(uncompress(rawcfg, &rawsize, copydata + copymapsize, copycfgsizegz) == Z_OK && rawsize - copycfgsize == 0)
                fwrite(rawcfg, 1, copycfgsize, fp);
            fclose(fp);
            DELETEA(rawcfg);
            written = true;
        }
    }
    return written;
}

ENetPacket *getmapserv(int n)
{
    if(!copydata || !mapavailable(smapname)) return NULL;
    ENetPacket *packet = enet_packet_create(NULL, MAXTRANS + copysize, ENET_PACKET_FLAG_RELIABLE);
    ucharbuf p(packet->data, packet->dataLength);
    putint(p, SV_RECVMAP);
    sendstring(copyname, p);
    putint(p, copymapsize);
    putint(p, copycfgsize);
    putint(p, copycfgsizegz);
    p.put(copydata, copysize);
    enet_packet_resize(packet, p.length());
    return packet;
}

// provide maps by the server

#define GZBUFSIZE ((MAXCFGFILESIZE * 11) / 10)

void getservermap(void)
{
    static uchar *gzbuf = NULL;
    string cgzname, cfgname;
    int cgzsize, cfgsize, cfgsizegz;
    const char *name = behindpath(smapname);   // no paths allowed here

    if(!gzbuf) gzbuf = new uchar[GZBUFSIZE];
    if(!gzbuf) return;
    if(!strcmp(name, behindpath(copyname))) return;
    s_sprintf(cgzname)(SERVERMAP_PATH "%s.cgz", name);
    path(cgzname);
    if(fileexists(cgzname, "r"))
    {
        s_sprintf(cfgname)(SERVERMAP_PATH "%s.cfg", name);
    }
    else
    {
        s_sprintf(cgzname)(SERVERMAP_PATH_INCOMING "%s.cgz", name);
        path(cgzname);
        s_sprintf(cfgname)(SERVERMAP_PATH_INCOMING "%s.cfg", name);
    }
    path(cfgname);
    uchar *cgzdata = (uchar *)loadfile(cgzname, &cgzsize);
    uchar *cfgdata = (uchar *)loadfile(cfgname, &cfgsize);
    if(cgzdata && cfgsize < MAXCFGFILESIZE)
    {
        uLongf gzbufsize = GZBUFSIZE;
        if(!cfgdata || compress2(gzbuf, &gzbufsize, cfgdata, cfgsize, 9) != Z_OK)
        {
            cfgsize = 0;
            gzbufsize = 0;
        }
        cfgsizegz = (int) gzbufsize;
        if(cgzsize + cfgsizegz < MAXMAPSENDSIZE)
        {
            s_strcpy(copyname, name);
            copymapsize = cgzsize;
            copycfgsize = cfgsize;
            copycfgsizegz = cfgsizegz;
            copysize = cgzsize + cfgsizegz;
            DELETEA(copydata);
            copydata = new uchar[copysize];
            memcpy(copydata, cgzdata, cgzsize);
            memcpy(copydata + cgzsize, gzbuf, cfgsizegz);
            logger->writeline(log::info,"loaded map %s, %d + %d(%d) bytes.", cgzname, cgzsize, cfgsize, cfgsizegz);
        }
    }
    DELETEA(cgzdata);
    DELETEA(cfgdata);
}

void sendresume(client &c, bool broadcast)
{
    sendf(broadcast ? -1 : c.clientnum, 1, "rxii9vvi", broadcast ? c.clientnum : -1, SV_RESUME,
            c.clientnum,
            c.state.state,
            c.state.lifesequence,
            c.state.gunselect,
            c.state.flagscore,
            c.state.frags,
            c.state.deaths,
            c.state.health,
            c.state.armour,
            NUMGUNS, c.state.ammo,
            NUMGUNS, c.state.mag,
            -1);
}

void sendinits2c(client &c)
{
    sendf(c.clientnum, 1, "ri5", SV_INITS2C, c.clientnum, PROTOCOL_VERSION, c.salt, serverpassword[0] ? 1 : 0);
}

void welcomepacket(ucharbuf &p, int n, ENetPacket *packet, bool forcedeath)
{
    #define CHECKSPACE(n) \
    { \
        int space = (n); \
        if(p.remaining() < space) \
        { \
           enet_packet_resize(packet, packet->dataLength + max(MAXTRANS, space - p.remaining())); \
           p.buf = packet->data; \
           p.maxlen = (int)packet->dataLength; \
        } \
    }

    if(!smapname[0] && configsets.length()) nextcfgset(false);

    client *c = valid_client(n) ? clients[n] : NULL;
    int numcl = numclients();

    putint(p, SV_WELCOME);
    putint(p, smapname[0] && !m_demo ? numcl : -1);
    if(smapname[0] && !m_demo)
    {
        putint(p, SV_MAPCHANGE);
        sendstring(smapname, p);
        putint(p, smode);
        putint(p, mapavailable(smapname) ? 1 : 0);
        if(smode>1 || (smode==0 && numnonlocalclients()>0))
        {
            putint(p, SV_TIMEUP);
            putint(p, minremain);
        }
        if(numcl>1 || m_demo)
        {
            putint(p, SV_ITEMLIST);
            loopv(sents) if(sents[i].spawned)
            {
                putint(p, i);
                putint(p, sents[i].type);
                CHECKSPACE(256);
            }
            putint(p, -1);
        }
        if(m_flags)
        {
            CHECKSPACE(256);
            loopi(2) putflaginfo(p, i);
        }
    }
    if(c && c->type == ST_TCPIP && serveroperator() != -1) sendserveropinfo(n);
    if(numcl>1)
    {
        putint(p, SV_FORCETEAM);
        putint(p, freeteam(n));
        putint(p, 0);
    }
    if(c) c->lastforce = servmillis;
    bool restored = false;
    if(c)
    {
        if(c->type==ST_TCPIP)
        {
            savedscore *sc = findscore(*c, false);
            if(sc)
            {
                sc->restore(c->state);
                restored = true;
            }
        }

        CHECKSPACE(256);
        if(!canspawn(c, true) || forcedeath)
        {
            putint(p, SV_FORCEDEATH);
            putint(p, n);
            sendf(-1, 1, "ri2x", SV_FORCEDEATH, n, n);
        }
        else
        {
            clientstate &gs = c->state;
            spawnstate(c);
            putint(p, SV_SPAWNSTATE);
            putint(p, gs.lifesequence);
            putint(p, gs.health);
            putint(p, gs.armour);
            putint(p, gs.primary);
            putint(p, gs.gunselect);
            putint(p, -1);
            loopi(NUMGUNS) putint(p, gs.ammo[i]);
            loopi(NUMGUNS) putint(p, gs.mag[i]);
            gs.lastspawn = gamemillis;
        }
    }
    if(clients.length()>1 || restored)
    {
        putint(p, SV_RESUME);
        loopv(clients)
        {
            client &c = *clients[i];
            if(c.type!=ST_TCPIP || (c.clientnum==n && !restored)) continue;
            CHECKSPACE(256);
            putint(p, c.clientnum);
            putint(p, c.state.state);
            putint(p, c.state.lifesequence);
            putint(p, c.state.gunselect);
            putint(p, c.state.flagscore);
            putint(p, c.state.frags);
            putint(p, c.state.deaths);
            putint(p, c.state.health);
            putint(p, c.state.armour);
            loopi(NUMGUNS) putint(p, c.state.ammo[i]);
            loopi(NUMGUNS) putint(p, c.state.mag[i]);
        }
        putint(p, -1);
    }
    putint(p, SV_AUTOTEAM);
    putint(p, autoteam ? 1 : 0);
    if(motd[0])
    {
        CHECKSPACE(5+2*(int)strlen(motd)+1);
        putint(p, SV_TEXT);
        sendstring(motd, p);
    }

    #undef CHECKSPACE
}

void sendwelcome(client *cl, int chan, bool forcedeath)
{
    ENetPacket *packet = enet_packet_create(NULL, MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
    ucharbuf p(packet->data, packet->dataLength);
    welcomepacket(p, cl->clientnum, packet, forcedeath);
    enet_packet_resize(packet, p.length());
    sendpacket(cl->clientnum, chan, packet);
    if(!packet->referenceCount) enet_packet_destroy(packet);
}

int checktype(int type, client *cl)
{
    if(cl && cl->type==ST_LOCAL) return type;
    // only allow edit messages in coop-edit mode
    static int edittypes[] = { SV_EDITENT, SV_EDITH, SV_EDITT, SV_EDITS, SV_EDITD, SV_EDITE, SV_NEWMAP };
    if(cl && smode!=1) loopi(sizeof(edittypes)/sizeof(int)) if(type == edittypes[i]) return -1;
    // server only messages
    static int servtypes[] = { SV_INITS2C, SV_MAPRELOAD, SV_SERVMSG, SV_GIBDAMAGE, SV_DAMAGE,
                        SV_HITPUSH, SV_SHOTFX, SV_DIED, SV_SPAWNSTATE, SV_FORCEDEATH, SV_ITEMACC,
                        SV_ITEMSPAWN, SV_TIMEUP, SV_CDIS, SV_PONG, SV_RESUME,
                        SV_FLAGINFO, SV_FLAGMSG, SV_FLAGCNT,
                        SV_ARENAWIN, SV_SENDDEMOLIST, SV_SENDDEMO, SV_DEMOPLAYBACK, SV_CLIENT,
                        SV_CALLVOTESUC, SV_CALLVOTEERR, SV_VOTERESULT, SV_WHOISINFO };
    if(cl) loopi(sizeof(servtypes)/sizeof(int)) if(type == servtypes[i]) return -1;
    return type;
}

// server side processing of updates: does very little and most state is tracked client only
// could be extended to move more gameplay to server (at expense of lag)

void process(ENetPacket *packet, int sender, int chan)   // sender may be -1
{
    ucharbuf p(packet->data, packet->dataLength);
    char text[MAXTRANS];
    client *cl = sender>=0 ? clients[sender] : NULL;
    pwddetail pd;
    int type;

    if(cl && !cl->isauthed)
    {
        int clientrole = CR_DEFAULT;

        if(chan==0) return;
        else if(chan!=1 || getint(p)!=SV_CONNECT) disconnect_client(sender, DISC_TAGT);
        else
        {
            getstring(text, p);
            filtertext(text, text, 0, MAXNAMELEN);
            if(!text[0]) s_strcpy(text, "unarmed");
            s_strncpy(cl->name, text, MAXNAMELEN+1);

            getstring(text, p);
            int wantrole = getint(p);
            cl->state.nextprimary = getint(p);
            bool banned = isbanned(sender);
            bool srvfull = numnonlocalclients() > maxclients;
            bool srvprivate = mastermode == MM_PRIVATE;
            if(checkadmin(cl->name, text, cl->salt, &pd) && (!pd.denyadmin || (banned && !srvfull && !srvprivate))) // pass admins always through
            {
                bool banremoved = false;
                cl->isauthed = true;
                if(!pd.denyadmin && wantrole == CR_ADMIN) clientrole = CR_ADMIN;
                if(banned)
                {
                    loopv(bans) if(bans[i].address.host == cl->peer->address.host) { banremoved = true; bans.remove(i); break; } // remove admin bans
                }
                if(srvfull)
                {
                    loopv(clients) if(i != sender && clients[i]->type==ST_TCPIP)
                    {
                        disconnect_client(i, DISC_MAXCLIENTS); // disconnect someone else to fit maxclients again
                        break;
                    }
                }
                logger->writeline(log::info, "[%s] logged in using the admin password in line %d%s", cl->hostname, pd.line, banremoved ? ", (ban removed)" : "");
            }
            else if(serverpassword[0])
            {
                if(!strcmp(genpwdhash(cl->name, serverpassword, cl->salt), text))
                {
                    cl->isauthed = true;
                    logger->writeline(log::info, "[%s] client logged in (using serverpassword)", cl->hostname);
                }
                else disconnect_client(sender, DISC_WRONGPW);
            }
            else if(srvprivate) disconnect_client(sender, DISC_MASTERMODE);
            else if(srvfull) disconnect_client(sender, DISC_MAXCLIENTS);
            else if(banned) disconnect_client(sender, DISC_BANREFUSE);
            else cl->isauthed = true;
        }
        if(!cl->isauthed) return;

        if(cl->type==ST_TCPIP)
        {
            loopv(clients) if(i != sender)
            {
                client *dup = clients[i];
                if(dup->type==ST_TCPIP && dup->peer->address.host==cl->peer->address.host && dup->peer->address.port==cl->peer->address.port)
                    disconnect_client(i, DISC_DUP);
            }
        }

        sendwelcome(cl);
        if(findscore(*cl, false)) sendresume(*cl, true);
        if(clientrole != CR_DEFAULT) changeclientrole(sender, clientrole, NULL, true);
    }

    if(packet->flags&ENET_PACKET_FLAG_RELIABLE) reliablemessages = true;

    #define QUEUE_MSG { if(cl->type==ST_TCPIP) while(curmsg<p.length()) cl->messages.add(p.buf[curmsg++]); }
    #define QUEUE_BUF(size, body) { \
        if(cl->type==ST_TCPIP) \
        { \
            curmsg = p.length(); \
            ucharbuf buf = cl->messages.reserve(size); \
            { body; } \
            cl->messages.addbuf(buf); \
        } \
    }
    #define QUEUE_INT(n) QUEUE_BUF(5, putint(buf, n))
    #define QUEUE_UINT(n) QUEUE_BUF(4, putuint(buf, n))
    #define QUEUE_STR(text) QUEUE_BUF(2*(int)strlen(text)+1, sendstring(text, buf))
    #define MSG_PACKET(packet) \
        ENetPacket *packet = enet_packet_create(NULL, 16 + p.length() - curmsg, ENET_PACKET_FLAG_RELIABLE); \
        ucharbuf buf(packet->data, packet->dataLength); \
        putint(buf, SV_CLIENT); \
        putint(buf, cl->clientnum); \
        putuint(buf, p.length() - curmsg); \
        buf.put(&p.buf[curmsg], p.length() - curmsg); \
        enet_packet_resize(packet, buf.length());

    int curmsg;
    while((curmsg = p.length()) < p.maxlen)
    {
        type = checktype(getint(p), cl);

        #ifdef _DEBUG
        if(type!=SV_POS && type!=SV_CLIENTPING && type!=SV_PING && type!=SV_CLIENT)
        {
            DEBUGVAR(cl->name);
            ASSERT(type>=0 && type<SV_NUM);
            DEBUGVAR(messagenames[type]);
            protocoldebug(true);
        }
        else protocoldebug(false);
        #endif

        switch(type)
        {
            case SV_TEAMTEXT:
                getstring(text, p);
                filtertext(text, text);
                if(!spamdetect(cl, text))
                {
                    logger->writeline(log::info, "[%s] %s says to team %s: '%s'", cl->hostname, cl->name, cl->team, text);
                    sendteamtext(text, sender);
                }
                else
                {
                    logger->writeline(log::info, "[%s] %s says to team %s: '%s', SPAM detected", cl->hostname, cl->name, cl->team, text);
                    sendservmsg("\f3please do not spam", sender);
                }
                break;

            case SV_TEXT:
            {
                int mid1 = curmsg, mid2 = p.length();
                getstring(text, p);
                filtertext(text, text);
                if(!spamdetect(cl, text))
                {
                    logger->writeline(log::info, "[%s] %s says: '%s'", cl->hostname, cl->name, text);
                    if(cl->type==ST_TCPIP) while(mid1<mid2) cl->messages.add(p.buf[mid1++]);
                    QUEUE_STR(text);
                }
                else
                {
                    logger->writeline(log::info, "[%s] %s says: '%s', SPAM detected", cl->hostname, cl->name, text);
                    sendservmsg("\f3please do not spam", sender);
                }
                break;
            }

            case SV_VOICECOM:
                getint(p);
                QUEUE_MSG;
                break;

            case SV_VOICECOMTEAM:
                sendvoicecomteam(getint(p), sender);
                break;

            case SV_INITC2S:
            {
                QUEUE_MSG;
                getstring(text, p);
                filtertext(text, text, 0, MAXNAMELEN);
                if(!text[0]) s_strcpy(text, "unarmed");
                QUEUE_STR(text);
                if(strcmp(cl->name, text)) logger->writeline(log::info,"[%s] %s changed his name to %s", cl->hostname, cl->name, text);
                s_strncpy(cl->name, text, MAXNAMELEN+1);
                getstring(text, p);
                filtertext(cl->team, text, 0, MAXTEAMLEN);
                QUEUE_STR(text);
                cl->skin = getint(p);
                QUEUE_MSG;
                break;
            }

            case SV_ITEMLIST:
            {
                int n;
                while((n = getint(p))!=-1)
                {
                    server_entity se = { getint(p), false, 0 };
                    if(notgotitems)
                    {
                        while(sents.length()<=n) sents.add(se);
                        sents[n].spawned = true;
                    }
                }
                notgotitems = false;
                break;
            }

            case SV_SPAWNLIST:
            {
                if(getint(p) > 0)
                {
                    loopi(3) clnumspawn[i] = getint(p);
                    loopi(2) clnumflagspawn[i] = getint(p);
                }
                QUEUE_MSG;
                break;
            }

            case SV_ITEMPICKUP:
            {
                int n = getint(p);
                gameevent &pickup = cl->addevent();
                pickup.type = GE_PICKUP;
                pickup.pickup.ent = n;
                break;
            }

            case SV_WEAPCHANGE:
            {
                int gunselect = getint(p);
                if(gunselect<0 && gunselect>=NUMGUNS) break;
                cl->state.gunselect = gunselect;
                QUEUE_MSG;
                break;
            }

            case SV_PRIMARYWEAP:
            {
                int nextprimary = getint(p);
                if(nextprimary<0 && nextprimary>=NUMGUNS) break;
                cl->state.nextprimary = nextprimary;
                break;
            }

            case SV_CHANGETEAM:
                if(cl->state.state==CS_ALIVE)
                {
                    cl->state.state = CS_DEAD;
                    cl->state.respawn();
                    sendf(-1, 1, "rii", SV_FORCEDEATH, cl->clientnum);
                }
                break;

            case SV_TRYSPAWN:
                if(cl->state.state!=CS_DEAD || cl->state.lastspawn>=0 || !canspawn(cl)) break;
                if(cl->state.lastdeath) cl->state.respawn();
                sendspawn(cl);
                break;

            case SV_SPAWN:
            {
                int ls = getint(p), gunselect = getint(p);
                if((cl->state.state!=CS_ALIVE && cl->state.state!=CS_DEAD) || ls!=cl->state.lifesequence || cl->state.lastspawn<0 || gunselect<0 || gunselect>=NUMGUNS) break;
                cl->state.lastspawn = -1;
                cl->state.state = CS_ALIVE;
                cl->state.gunselect = gunselect;
                QUEUE_BUF(5*(5 + 2*NUMGUNS),
                {
                    putint(buf, SV_SPAWN);
                    putint(buf, cl->state.lifesequence);
                    putint(buf, cl->state.health);
                    putint(buf, cl->state.armour);
                    putint(buf, cl->state.gunselect);
                    loopi(NUMGUNS) putint(buf, cl->state.ammo[i]);
                    loopi(NUMGUNS) putint(buf, cl->state.mag[i]);
                });
                break;
            }

            case SV_SUICIDE:
            {
                gameevent &suicide = cl->addevent();
                suicide.type = GE_SUICIDE;
                break;
            }

            case SV_SHOOT:
            {
                gameevent &shot = cl->addevent();
                shot.type = GE_SHOT;
                #define seteventmillis(event) \
                { \
                    event.id = getint(p); \
                    if(!cl->timesync || (cl->events.length()==1 && cl->state.waitexpired(gamemillis))) \
                    { \
                        cl->timesync = true; \
                        cl->gameoffset = gamemillis - event.id; \
                        event.millis = gamemillis; \
                    } \
                    else event.millis = cl->gameoffset + event.id; \
                }
                seteventmillis(shot.shot);
                shot.shot.gun = getint(p);
                loopk(3) shot.shot.from[k] = getint(p)/DMF;
                loopk(3) shot.shot.to[k] = getint(p)/DMF;
                int hits = getint(p);
                loopk(hits)
                {
                    gameevent &hit = cl->addevent();
                    hit.type = GE_HIT;
                    hit.hit.target = getint(p);
                    hit.hit.lifesequence = getint(p);
                    hit.hit.info = getint(p);
                    loopk(3) hit.hit.dir[k] = getint(p)/DNF;
                }
                break;
            }

            case SV_EXPLODE:
            {
                gameevent &exp = cl->addevent();
                exp.type = GE_EXPLODE;
                seteventmillis(exp.explode);
                exp.explode.gun = getint(p);
                exp.explode.id = getint(p);
                int hits = getint(p);
                loopk(hits)
                {
                    gameevent &hit = cl->addevent();
                    hit.type = GE_HIT;
                    hit.hit.target = getint(p);
                    hit.hit.lifesequence = getint(p);
                    hit.hit.dist = getint(p)/DMF;
                    loopk(3) hit.hit.dir[k] = getint(p)/DNF;
                }
                break;
            }

            case SV_AKIMBO:
            {
                gameevent &akimbo = cl->addevent();
                akimbo.type = GE_AKIMBO;
                seteventmillis(akimbo.akimbo);
                break;
            }

            case SV_RELOAD:
            {
                gameevent &reload = cl->addevent();
                reload.type = GE_RELOAD;
                seteventmillis(reload.reload);
                reload.reload.gun = getint(p);
                break;
            }

            case SV_PING:
                sendf(sender, 1, "ii", SV_PONG, getint(p));
                break;

            case SV_POS:
            {
                int cn = getint(p);
                if(cn!=sender)
                {
                    disconnect_client(sender, DISC_CN);
    #ifndef STANDALONE
                    conoutf("ERROR: invalid client (msg %i)", type);
    #endif
                    return;
                }
                loopi(3) clients[cn]->state.o[i] = getuint(p)/DMF;
                getuint(p);
                loopi(5) getint(p);
                getuint(p);
                if(cl->type==ST_TCPIP && (cl->state.state==CS_ALIVE || cl->state.state==CS_EDITING))
                {
                    cl->position.setsizenodelete(0);
                    while(curmsg<p.length()) cl->position.add(p.buf[curmsg++]);
                }
                break;
            }

            case SV_NEXTMAP:
            {
                getstring(text, p);
                filtertext(text, text);
                int mode = getint(p);
                if(mapreload || numclients() == 1) resetmap(text, mode);
                break;
            }

            case SV_SENDMAP:
            {
                getstring(text, p);
                filtertext(text, text);
                int mapsize = getint(p);
                int cfgsize = getint(p);
                int cfgsizegz = getint(p);
                if(p.remaining() < mapsize + cfgsizegz)
                {
                    p.forceoverread();
                    break;
                }
                if(sendmapserv(sender, text, mapsize, cfgsize, cfgsizegz, &p.buf[p.len]))
                {
                    logger->writeline(log::info,"[%s] %s sent map %s, %d + %d(%d) bytes written",
                                clients[sender]->hostname, clients[sender]->name, text, mapsize, cfgsize, cfgsizegz);
                }
                p.len += mapsize + cfgsizegz;
                break;
            }

            case SV_RECVMAP:
            {
                ENetPacket *mappacket = getmapserv(cl->clientnum);
                if(mappacket)
                {
                    resetflag(cl->clientnum); // drop ctf flag
                    // save score
                    savedscore *sc = findscore(*cl, true);
                    if(sc) sc->save(cl->state);
                    // resend state properly
                    sendpacket(cl->clientnum, 2, mappacket);
                    cl->mapchange();
                    sendwelcome(cl, 2, true);

                }
                else sendservmsg("no map to get", cl->clientnum);
                break;
            }

		    case SV_FLAGACTION:
		    {
		        int action = getint(p);
		        int flag = getint(p);
		        if(!m_flags || flag < 0 || flag > 1 || action < 0 || action > FA_NUM) break;
			    flagaction(flag, action, sender);
			    break;
		    }

            case SV_SETADMIN:
		    {
			    bool claim = getint(p) != 0;
			    getstring(text, p);
                changeclientrole(sender, claim ? CR_ADMIN : CR_DEFAULT, text);
			    break;
		    }

            case SV_CALLVOTE:
            {
                voteinfo *vi = new voteinfo;
                int type = getint(p);
                switch(type)
                {
                    case SA_MAP:
                    {
                        getstring(text, p);
                        filtertext(text, text);
                        int mode = getint(p);
                        if(mode==GMODE_DEMO) vi->action = new demoplayaction(text);
                        else vi->action = new mapaction(newstring(text), mode);
                        break;
                    }
                    case SA_KICK:
                        vi->action = new kickaction(getint(p));
                        break;
                    case SA_BAN:
                        vi->action = new banaction(getint(p));
                        break;
                    case SA_REMBANS:
                        vi->action = new removebansaction();
                        break;
                    case SA_MASTERMODE:
                        vi->action = new mastermodeaction(getint(p));
                        break;
                    case SA_AUTOTEAM:
                        vi->action = new autoteamaction(getint(p) > 0);
                        break;
                    case SA_SHUFFLETEAMS:
                        vi->action = new shuffleteamaction();
                        break;
                    case SA_FORCETEAM:
                        vi->action = new forceteamaction(getint(p));
                        break;
                    case SA_GIVEADMIN:
                        vi->action = new giveadminaction(getint(p));
                        break;
                    case SA_RECORDDEMO:
                        vi->action = new recorddemoaction(getint(p)!=0);
                        break;
                    case SA_STOPDEMO:
                        vi->action = new stopdemoaction();
                        break;
                    case SA_CLEARDEMOS:
                        vi->action = new cleardemosaction(getint(p));
                        break;
                    case SA_SERVERDESC:
                        getstring(text, p);
                        filtertext(text, text);
                        vi->action = new serverdescaction(newstring(text), sender);
                        break;
                }
                vi->owner = sender;
                vi->callmillis = servmillis;
                MSG_PACKET(msg);
                if(!callvote(vi, msg)) delete vi;
                if(!msg->referenceCount) enet_packet_destroy(msg);
                break;
            }

            case SV_VOTE:
            {
                int n = getint(p);
                MSG_PACKET(msg);
                vote(sender, n, msg);
                if(valid_client(sender) && !msg->referenceCount) enet_packet_destroy(msg); // check sender existence first because he might have been disconnected due to a vote
                break;
            }

            case SV_WHOIS:
            {
                sendwhois(sender, getint(p));
                break;
            }

            case SV_LISTDEMOS:
                listdemos(sender);
                break;

            case SV_GETDEMO:
                senddemo(sender, getint(p));
                break;

            case SV_EXTENSION:
            {
                // AC server extensions
                //
                // rules:
                // 1. extensions MUST NOT modify gameplay or the beavior of the game in any way
                // 2. extensions may ONLY be used to extend or automate server administration tasks
                // 3. extensions may ONLY operate on the server and must not send any additional data to the connected clients
                // 4. extensions not adhering to these rules may cause the hosting server being banned from the masterserver
                //
                // also note that there is no guarantee that custom extensions will work in future AC versions


                getstring(text, p, 64);
                char *ext = text;   // extension specifier in the form of OWNER::EXTENSION, see sample below
                int n = getint(p);  // length of data after the specifier
                if(n > 50) return;

                // sample
                if(!strcmp(ext, "driAn::writelog"))
                {
                    // owner:       driAn - root@sprintf.org
                    // extension:   writelog - WriteLog v1.0
                    // description: writes a custom string to the server log
                    // access:      requires admin privileges
                    // usage:       /serverextension driAn::writelog "your log message here.."

                    getstring(text, p, n);
                    if(valid_client(sender) && clients[sender]->role==CR_ADMIN && logger)
                        logger->writeline(log::info, text);
                }
                // else if()

                // add other extensions here

                else for(; n > 0; n--) getint(p); // ignore unknown extensions

                break;
            }

            default:
            {
                int size = msgsizelookup(type);
                if(size==-1) { if(sender>=0) disconnect_client(sender, DISC_TAGT); return; }
                loopi(size-1) getint(p);
                QUEUE_MSG;
                break;
            }
        }
    }

    if(p.overread() && sender>=0) disconnect_client(sender, DISC_EOP);

    #ifdef _DEBUG
    protocoldebug(false);
    #endif
}

void localclienttoserver(int chan, ENetPacket *packet)
{
    process(packet, 0, chan);
    if(!packet->referenceCount) enet_packet_destroy(packet);
}

client &addclient()
{
    client *c = NULL;
    loopv(clients) if(clients[i]->type==ST_EMPTY) { c = clients[i]; break; }
    if(!c)
    {
        c = new client;
        c->clientnum = clients.length();
        clients.add(c);
    }
    c->reset();
    return *c;
}

void checkintermission()
{
    if(minremain>0)
    {
        minremain = gamemillis>=gamelimit || forceintermission ? 0 : (gamelimit - gamemillis + 60000 - 1)/60000;
        sendf(-1, 1, "ri2", SV_TIMEUP, minremain);
    }
    if(!interm && minremain<=0) interm = gamemillis+10000;
    forceintermission = false;
}

void resetserverifempty()
{
    loopv(clients) if(clients[i]->type!=ST_EMPTY) return;
    resetmap("", 0, 10, false);
    mastermode = MM_OPEN;
    autoteam = true;
    nextmapname[0] = '\0';
}

void sendworldstate()
{
    static enet_uint32 lastsend = 0;
    if(clients.empty()) return;
    enet_uint32 curtime = enet_time_get()-lastsend;
    if(curtime<40) return;
    bool flush = buildworldstate();
    lastsend += curtime - (curtime%40);
    if(flush) enet_host_flush(serverhost);
    if(demorecord) recordpackets = true; // enable after 'old' worldstate is sent
}

void rereadcfgs(void)
{
    static int cfgupdate;
    if(!cfgupdate || servmillis - cfgupdate > 10 * 60 * 1000)
    {
        cfgupdate = servmillis;
        readscfg(NULL);
        readblacklist(NULL);
        readpwdfile(NULL);
    }
}

void loggamestatus(const char *reason)
{
    int fragscore[2] = {0, 0}, flagscore[2] = {0, 0}, pnum[2] = {0, 0}, n;
    string text1, text2;
    s_sprintf(text1)("%d minutes remaining", minremain);
    logger->writeline(log::info, "\nGame status: %s on %s, %s, %s%c %s",
                      modestr(gamemode), smapname, reason ? reason : text1, mmfullname(mastermode), custom_servdesc ? ',' : '\0', servdesc_cur);
    logger->writeline(log::info, "cn name             %sfrag death %srole    host", m_teammode ? "team " : "", m_flags ? "flags  " : "");
    loopv(clients)
    {
        if(clients[i]->type == ST_EMPTY || !clients[i]->name[0]) continue;
        s_sprintf(text1)("%2d %-16s%c%-4s", clients[i]->clientnum, clients[i]->name, m_teammode ? ' ' : '\0', clients[i]->team);
        s_sprintf(text2)(" %4d %5d%c%5d", clients[i]->state.frags, clients[i]->state.deaths, m_flags ? ' ' : '\0', clients[i]->state.flagscore);
        logger->writeline(log::info, "%s%s %-6s  %s", text1, text2, clients[i]->role == CR_ADMIN ? "admin" : "normal", clients[i]->hostname);
        n = team_int(clients[i]->team);
        flagscore[n] += clients[i]->state.flagscore;
        fragscore[n] += clients[i]->state.frags;
        pnum[n] += 1;
    }
    if(m_teammode)
    {
        loopi(2) logger->writeline(log::info, "Team %4s:%3d players,%5d frags%c%5d flags", team_string(i), pnum[i], fragscore[i], m_flags ? ',' : '\0', flagscore[i]);
    }
    logger->writeline(log::info, "");
}

void serverslice(uint timeout)   // main server update, called from cube main loop in sp, or dedicated server loop
{
#ifdef STANDALONE
    int nextmillis = (int)enet_time_get();
    if(svcctrl) svcctrl->keepalive();
#else
    int nextmillis = isdedicated ? (int)enet_time_get() : lastmillis;
#endif
    int diff = nextmillis - servmillis;
    gamemillis += diff;
    servmillis = nextmillis;

    if(m_demo) readdemo();

    if(minremain>0)
    {
        processevents();
        checkitemspawns(diff);
        bool ktfflagingame = false;
        if(m_flags) loopi(2)
        {
            sflaginfo &f = sflaginfos[i];
            if(f.state == CTFF_DROPPED && gamemillis-f.lastupdate > (m_ctf ? 30000 : 10000)) flagaction(i, FA_RESET, -1);
            if(m_htf && f.state == CTFF_INBASE && gamemillis-f.lastupdate > (clnumflagspawn[0] && clnumflagspawn[1] ? 10000 : 1000))
            {
                htf_forceflag(i);
            }
            if(m_ktf && f.state == CTFF_STOLEN && gamemillis-f.lastupdate > 15000)
            {
                flagaction(i, FA_SCORE, -1);
            }
            if(f.state == CTFF_INBASE || f.state == CTFF_STOLEN) ktfflagingame = true;
        }
        if(m_ktf && !ktfflagingame) flagaction(rnd(2), FA_RESET, -1); // ktf flag watchdog
        if(m_arena) arenacheck();
    }

    if(curvote)
    {
        if(!curvote->isalive()) curvote->evaluate(true);
        if(curvote->result!=VOTE_NEUTRAL) DELETEP(curvote);
    }

    int nonlocalclients = numnonlocalclients();

    if(forceintermission || ((smode>1 || (gamemode==0 && nonlocalclients)) && gamemillis-diff>0 && gamemillis/60000!=(gamemillis-diff)/60000))
        checkintermission();
    if(interm && gamemillis>interm)
    {
        loggamestatus("game finished");
        if(demorecord) enddemorecord();
        interm = 0;

        //start next game
        if(nextmapname[0]) resetmap(nextmapname, nextgamemode);
        else if(configsets.length()) nextcfgset();
        else loopv(clients) if(clients[i]->type!=ST_EMPTY)
        {
            sendf(i, 1, "rii", SV_MAPRELOAD, 0);    // ask a client to trigger map reload
            mapreload = true;
            break;
        }
    }

    resetserverifempty();

    if(!isdedicated) return;     // below is network only

    serverms(smode, numclients(), minremain, smapname, servmillis, serverhost->address);

    if(autoteam && m_teammode && !m_arena && !interm && servmillis - lastfillup > 5000 && refillteams()) lastfillup = servmillis;

    if(servmillis-laststatus>60*1000)   // display bandwidth stats, useful for server ops
    {
        laststatus = servmillis;
        rereadcfgs();
		if(nonlocalclients || bsend || brec)
		{
		    if(nonlocalclients) loggamestatus(NULL);

		    time_t rawtime;
		    struct tm * timeinfo;
		    char buffer [80];

		    time (&rawtime);
		    timeinfo = localtime(&rawtime);
		    strftime (buffer,80,"%d-%m-%Y %H:%M:%S",timeinfo);
            logger->writeline(log::info, "Status at %s: %d remote clients, %.1f send, %.1f rec (K/sec)", buffer, nonlocalclients, bsend/60.0f/1024, brec/60.0f/1024);
		}
        bsend = brec = 0;
    }

    ENetEvent event;
    bool serviced = false;
    while(!serviced)
    {
        if(enet_host_check_events(serverhost, &event) <= 0)
        {
            if(enet_host_service(serverhost, &event, timeout) <= 0) break;
            serviced = true;
        }
        switch(event.type)
        {
            case ENET_EVENT_TYPE_CONNECT:
            {
                client &c = addclient();
                c.type = ST_TCPIP;
                c.peer = event.peer;
                c.peer->data = (void *)(size_t)c.clientnum;
                c.connectmillis = servmillis;
                c.salt = rand()*((servmillis%1000)+1);
				char hn[1024];
				s_strcpy(c.hostname, (enet_address_get_host_ip(&c.peer->address, hn, sizeof(hn))==0) ? hn : "unknown");
                logger->writeline(log::info,"[%s] client connected", c.hostname);
                sendinits2c(c);
				break;
            }

            case ENET_EVENT_TYPE_RECEIVE:
			{
                brec += (int)event.packet->dataLength;
				int cn = (int)(size_t)event.peer->data;
				if(valid_client(cn)) process(event.packet, cn, event.channelID);
                if(event.packet->referenceCount==0) enet_packet_destroy(event.packet);
                break;
			}

            case ENET_EVENT_TYPE_DISCONNECT:
            {
				int cn = (int)(size_t)event.peer->data;
				if(!valid_client(cn)) break;
                disconnect_client(cn);
                break;
            }

            default:
                break;
        }
    }
    sendworldstate();
}

void cleanupserver()
{
    if(serverhost) enet_host_destroy(serverhost);
    if(svcctrl)
    {
        svcctrl->stop();
        DELETEP(svcctrl);
    }
    if(logger) DELETEP(logger);
}

void extinfo_cnbuf(ucharbuf &p, int cn)
{
    if(cn == -1) // add all available player ids
    {
        loopv(clients) if(clients[i]->type != ST_EMPTY)
            putint(p,clients[i]->clientnum);
    }
    else if(valid_client(cn)) // add single player only
    {
        putint(p,clients[cn]->clientnum);
    }
}

void extinfo_statsbuf(ucharbuf &p, int pid, int bpos, ENetSocket &pongsock, ENetAddress &addr, ENetBuffer &buf, int len)
{
    loopv(clients)
    {
        if(clients[i]->type == ST_EMPTY) continue;
        if(pid>-1 && clients[i]->clientnum!=pid) continue;

        putint(p,EXT_PLAYERSTATS_RESP_STATS);  // send player stats following
        putint(p,clients[i]->clientnum);  //add player id
        sendstring(clients[i]->name,p);         //Name
        sendstring(clients[i]->team,p);         //Team
        putint(p,clients[i]->state.frags);      //Frags
        putint(p,clients[i]->state.deaths);     //Death
        putint(p,clients[i]->state.teamkills);  //Teamkills
        putint(p,clients[i]->state.damage*100/max(clients[i]->state.shotdamage,1)); //Accuracy
        putint(p,clients[i]->state.health);     //Health
        putint(p,clients[i]->state.armour);     //Armour
        putint(p,clients[i]->state.gunselect);  //Gun selected
        putint(p,clients[i]->role);             //Role
        putint(p,clients[i]->state.state);      //State (Alive,Dead,Spawning,Lagged,Editing)
        uint ip = clients[i]->peer->address.host; // only 3 byte of the ip address (privacy protected)
        p.put((uchar*)&ip,3);

        buf.dataLength = len + p.length();
        enet_socket_send(pongsock, &addr, &buf, 1);

        if(pid>-1) break;
        p.len=bpos;
    }
}

void extinfo_teamscorebuf(ucharbuf &p)
{
    if(!m_teammode)
    {
        putint(p,EXT_ERROR); // send error
        putint(p,minremain);    //remaining play time
        return;
    }

    putint(p, m_teammode ? EXT_ERROR_NONE : EXT_ERROR);
    putint(p, minremain);
    putint(p, gamemode);
    if(!m_teammode) return;

    cvector teams;
    bool addteam;
    loopv(clients) if(clients[i]->type!=ST_EMPTY)
    {
        addteam = true;
        loopvj(teams)
        {
            if(strcmp(clients[i]->team,teams[j])==0 || !clients[i]->team[0])
            {
                addteam = false;
                break;
            }
        }
        if(addteam) teams.add(clients[i]->team);
    }

    loopv(teams)
    {
        sendstring(teams[i],p); //team
        int fragscore = 0;
        int flagscore = 0;
        loopvj(clients) if(clients[j]->type!=ST_EMPTY)
        {
            if(!(strcmp(clients[j]->team,teams[i])==0)) continue;
            fragscore += clients[j]->state.frags;
            flagscore += clients[j]->state.flagscore;
        }
        putint(p,fragscore); //add fragscore per team
        if(m_flags) //when capture mode
        {
            putint(p,flagscore); //add flagscore per team
        }
        else //all other team modes
        {
            putint(p,-1); //flagscore not available
        }
        putint(p,-1);
    }
}


#ifndef STANDALONE
void localdisconnect()
{
    loopv(clients) if(clients[i]->type==ST_LOCAL) clients[i]->zap();
}

void localconnect()
{
    client &c = addclient();
    c.type = ST_LOCAL;
    c.role = CR_ADMIN;
    s_strcpy(c.hostname, "local");
    sendinits2c(c);
}
#endif

void initserver(bool dedicated, int uprate, const char *sdesc, const char *sdesc_pre, const char *sdesc_suf, const char *ip, int serverport, const char *master, const char *passwd, int maxcl, const char *maprot, const char *adminpwd, const char *pwdfile, const char *blfile, const char *srvmsg, int kthreshold, int bthreshold, int permdemo, const char *voteperms, const char *demop)
{
    srand(time(NULL));

    if(serverport<=0) serverport = CUBE_DEFAULT_SERVER_PORT;
    if(passwd) s_strcpy(serverpassword, passwd);
    maxclients = maxcl > 0 ? min(maxcl, MAXCLIENTS) : DEFAULTCLIENTS;
    filterrichtext(servdesc_full, sdesc);
    filterservdesc(servdesc_full, servdesc_full);
    servermsinit(master ? master : AC_MASTER_URI, ip, CUBE_SERVINFO_PORT(serverport), servdesc_full, dedicated);
    s_strcpy(servdesc_cur, servdesc_full);
    filterrichtext(servdesc_pre, sdesc_pre);
    filterservdesc(servdesc_pre, servdesc_pre);
    filterrichtext(servdesc_suf, sdesc_suf);
    filterservdesc(servdesc_suf, servdesc_suf);
    s_strcpy(voteperm, voteperms && voteperms[0] ? voteperms : "");
    s_strcpy(demopath, demop && demop[0] ? demop : "");
    motd[0] = '\0';

    s_sprintfd(identity)("%s[%d]", ip && ip[0] ? ip : "local", serverport);
    logger = newlogger(identity);
    if(dedicated) logger->open(); // log on ded servers only
    logger->writeline(log::info, "logging local AssaultCube server now..");

    if((isdedicated = dedicated))
    {
        ENetAddress address = { ENET_HOST_ANY, serverport };
        if(*ip && enet_address_set_host(&address, ip)<0) logger->writeline(log::warning, "server ip not resolved!");
        serverhost = enet_host_create(&address, maxclients+1, 0, uprate);
        if(!serverhost) fatal("could not create server host");
        loopi(maxclients) serverhost->peers[i].data = (void *)-1;

        readscfg(maprot && maprot[0] ? maprot : "config/maprot.cfg");
        if(adminpwd && adminpwd[0]) adminpasswd = adminpwd;
        if(srvmsg && srvmsg[0]) filterrichtext(motd, srvmsg);
        kickthreshold = min(-1, kthreshold);
        banthreshold = min(-1, bthreshold);
        readpwdfile(pwdfile && pwdfile[0] ? pwdfile : "config/serverpwd.cfg");
        readblacklist(blfile && blfile[0] ? blfile : "config/serverblacklist.cfg");
        if(permdemo >= 0)
        {
            demoeverymatch = true;
            if(permdemo > 0) maxdemos = permdemo;
            if(verbose) logger->writeline(log::info, "recording demo of every game (holding up to %d in memory)", maxdemos);
        }
    }

    resetserverifempty();

    if(isdedicated)       // do not return, this becomes main loop
    {
        #ifdef WIN32
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        #endif
        logger->writeline(log::info, "dedicated server started, waiting for clients...\nCtrl-C to exit\n");
        atexit(enet_deinitialize);
        atexit(cleanupserver);
        enet_time_set(0);
        for(;;) serverslice(5);
    }
}

#ifdef STANDALONE

void localservertoclient(int chan, uchar *buf, int len) {}
void fatal(const char *s, ...)
{
    cleanupserver();
    s_sprintfdlv(msg,s,s);
    s_sprintfd(out)("AssaultCube fatal error: %s", msg);
    if(logger) logger->writeline(log::error, out);
    else puts(out);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    #ifdef WIN32
    //atexit((void (__cdecl *)(void))_CrtDumpMemoryLeaks);
    #ifndef _DEBUG
    #ifndef __GNUC__
    __try {
    #endif
    #endif
    #endif

    int uprate = 0, maxcl = DEFAULTCLIENTS, kthreshold = -5, bthreshold = -6, port = 0, permdemo = -1;
    const char *sdesc = "", *sdesc_pre = "", *sdesc_suf = "", *ip = "", *master = NULL, *passwd = "", *maprot = "", *admpwd = NULL, *pwdfile = NULL, *blfile = NULL, *srvmsg = NULL, *service = NULL, *voteperms = NULL, *demop = NULL;

    for(int i = 1; i<argc; i++)
    {
        char *a = &argv[i][2];
        if(argv[i][0]=='-') switch(argv[i][1])
        {
            case '-':
                if(!strncmp(argv[i], "--wizard", 8))
                {
                    return wizardmain(argc-1, argv+1);
                }
                break;
            case 'u': uprate = atoi(a); break;
            case 'n':
                switch(*a)
                {
                    case '1': sdesc_pre  = a + 1; break;
                    case '2': sdesc_suf  = a + 1; break;
                    default: sdesc  = a; break;
                }
                break;
            case 'i': ip     = a; break;
            case 'm': master = a; break;
            case 'p': passwd = a; break;
            case 'c': maxcl  = atoi(a); break;
            case 'r': maprot = a; break;
            case 'x': admpwd = a; break;
            case 'X': pwdfile = a; break;
            case 'B': blfile = a; break;
            case 'V': verbose = 1; break;
            case 'o': srvmsg = a; break;
            case 'k': kthreshold = atoi(a); break;
            case 'y': bthreshold = atoi(a); break;
            case 'S': service = a; break;
            case 'f': port = atoi(a); break;
            case 'D': permdemo = isdigit(*a) ? atoi(a) : 0; break;
            case 'P': voteperms = *a ? a : NULL; break;
            case 'W': demop = a; break;
            default: printf("WARNING: unknown commandline option\n");
        }
    }

    if(service && !svcctrl)
    {
        #ifdef WIN32
        svcctrl = new winservice(service);
        #endif
        if(svcctrl)
        {
            svcctrl->argc = argc; svcctrl->argv = argv;
            svcctrl->start();
        }
    }

    if(enet_initialize()<0) fatal("Unable to initialise network module");
    initserver(true, uprate, sdesc, sdesc_pre, sdesc_suf, ip, port, master, passwd, maxcl, maprot, admpwd, pwdfile, blfile, srvmsg, kthreshold, bthreshold, permdemo, voteperms, demop);
    return EXIT_SUCCESS;

    #if defined(WIN32) && !defined(_DEBUG) && !defined(__GNUC__)
    } __except(stackdumper(0, GetExceptionInformation()), EXCEPTION_CONTINUE_SEARCH) { return 0; }
    #endif
}
#endif

