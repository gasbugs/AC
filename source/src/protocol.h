#define MAXCLIENTS 256                  // in a multiplayer game, can be arbitrarily changed
#define DEFAULTCLIENTS 6
#define MAXTRANS 5000                   // max amount of data to swallow in 1 go
#define CUBE_SERVER_PORT 28763
#define CUBE_SERVINFO_PORT 28764
#define PROTOCOL_VERSION 1123            // bump when protocol changes
#define SAVEGAMEVERSION 1006               // bump if dynent/netprotocol changes or any other savegame/demo data bumped from 5

// network messages codes, c2s, c2c, s2c
enum
{
    SV_INITS2C = 0, SV_INITC2S, SV_POS, SV_TEXT, SV_SOUND, SV_CDIS,
    SV_GIBDIED, SV_DIED, SV_GIBDAMAGE, SV_DAMAGE, SV_SHOT, SV_FRAGS, SV_RESUME,
    SV_TIMEUP, SV_EDITENT, SV_MAPRELOAD, SV_ITEMACC,
    SV_MAPCHANGE, SV_ITEMSPAWN, SV_ITEMPICKUP, SV_DENIED,
    SV_PING, SV_PONG, SV_CLIENTPING, SV_GAMEMODE,
    SV_EDITH, SV_EDITT, SV_EDITS, SV_EDITD, SV_EDITE,
    SV_SENDMAP, SV_RECVMAP, SV_SERVMSG, SV_ITEMLIST, SV_WEAPCHANGE,
    SV_MODELSKIN,
    SV_FLAGPICKUP, SV_FLAGDROP, SV_FLAGRETURN, SV_FLAGSCORE, SV_FLAGINFO, SV_FLAGS, //EDIT: AH
    SV_GETMASTER, SV_MASTERCMD,
    SV_PWD,
    SV_EXT,
};

enum { MCMD_KICK = 0, MCMD_BAN, MCMD_REMBANS };

#define DMF 16.0f 
#define DAF 1.0f 
#define DVF 100.0f

enum { DISC_NONE = 0, DISC_EOP, DISC_CN, DISC_MKICK, DISC_MBAN, DISC_TAGT, DISC_BANREFUSE, DISC_WRONGPW, DISC_MLOGINFAIL, DISC_MAXCLIENTS };

