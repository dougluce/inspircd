/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  Inspire is copyright (C) 2002-2005 ChatSpike-Dev.
 *                       E-mail:
 *                <brain@chatspike.net>
 *           	  <Craig@chatspike.net>
 *     
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

/* $ModDesc: Povides a spanning tree server link protocol */

using namespace std;

#include <stdio.h>
#include <vector>
#include <deque>
#include "globals.h"
#include "inspircd_config.h"
#ifdef GCC3
#include <ext/hash_map>
#else
#include <hash_map>
#endif
#include "users.h"
#include "channels.h"
#include "modules.h"
#include "commands.h"
#include "socket.h"
#include "helperfuncs.h"
#include "inspircd.h"
#include "inspstring.h"
#include "hashcomp.h"
#include "message.h"
#include "xline.h"
#include "typedefs.h"
#include "cull_list.h"
#include "aes.h"

#ifdef GCC3
#define nspace __gnu_cxx
#else
#define nspace std
#endif

/*
 * The server list in InspIRCd is maintained as two structures
 * which hold the data in different ways. Most of the time, we
 * want to very quicky obtain three pieces of information:
 *
 * (1) The information on a server
 * (2) The information on the server we must send data through
 *     to actually REACH the server we're after
 * (3) Potentially, the child/parent objects of this server
 *
 * The InspIRCd spanning protocol provides easy access to these
 * by storing the data firstly in a recursive structure, where
 * each item references its parent item, and a dynamic list
 * of child items, and another structure which stores the items
 * hashed, linearly. This means that if we want to find a server
 * by name quickly, we can look it up in the hash, avoiding
 * any O(n) lookups. If however, during a split or sync, we want
 * to apply an operation to a server, and any of its child objects
 * we can resort to recursion to walk the tree structure.
 */

class ModuleSpanningTree;
static ModuleSpanningTree* TreeProtocolModule;

extern ServerConfig* Config;

extern std::vector<Module*> modules;
extern std::vector<ircd_module*> factory;
extern int MODCOUNT;

/* Any socket can have one of five states at any one time.
 * The LISTENER state indicates a socket which is listening
 * for connections. It cannot receive data itself, only incoming
 * sockets.
 * The CONNECTING state indicates an outbound socket which is
 * waiting to be writeable.
 * The WAIT_AUTH_1 state indicates the socket is outbound and
 * has successfully connected, but has not yet sent and received
 * SERVER strings.
 * The WAIT_AUTH_2 state indicates that the socket is inbound
 * (allocated by a LISTENER) but has not yet sent and received
 * SERVER strings.
 * The CONNECTED state represents a fully authorized, fully
 * connected server.
 */
enum ServerState { LISTENER, CONNECTING, WAIT_AUTH_1, WAIT_AUTH_2, CONNECTED };

/* We need to import these from the core for use in netbursts */
extern user_hash clientlist;
extern chan_hash chanlist;

/* Foward declarations */
class TreeServer;
class TreeSocket;

/* This variable represents the root of the server tree
 * (for all intents and purposes, it's us)
 */
TreeServer *TreeRoot;

Server* Srv;

/* This hash_map holds the hash equivalent of the server
 * tree, used for rapid linear lookups.
 */
typedef nspace::hash_map<std::string, TreeServer*> server_hash;
server_hash serverlist;

/* More forward declarations */
bool DoOneToOne(std::string prefix, std::string command, std::deque<std::string> &params, std::string target);
bool DoOneToAllButSender(std::string prefix, std::string command, std::deque<std::string> &params, std::string omit);
bool DoOneToMany(std::string prefix, std::string command, std::deque<std::string> &params);
bool DoOneToAllButSenderRaw(std::string data, std::string omit, std::string prefix, std::string command, std::deque<std::string> &params);
void ReadConfiguration(bool rebind);

/* Imported from xline.cpp for use during netburst */
extern std::vector<KLine> klines;
extern std::vector<GLine> glines;
extern std::vector<ZLine> zlines;
extern std::vector<QLine> qlines;
extern std::vector<ELine> elines;
extern std::vector<KLine> pklines;
extern std::vector<GLine> pglines;
extern std::vector<ZLine> pzlines;
extern std::vector<QLine> pqlines;
extern std::vector<ELine> pelines;

/* Each server in the tree is represented by one class of
 * type TreeServer. A locally connected TreeServer can
 * have a class of type TreeSocket associated with it, for
 * remote servers, the TreeSocket entry will be NULL.
 * Each server also maintains a pointer to its parent
 * (NULL if this server is ours, at the top of the tree)
 * and a pointer to its "Route" (see the comments in the
 * constructors below), and also a dynamic list of pointers
 * to its children which can be iterated recursively
 * if required. Creating or deleting objects of type
 * TreeServer automatically maintains the hash_map of
 * TreeServer items, deleting and inserting them as they
 * are created and destroyed.
 */

class TreeServer
{
	TreeServer* Parent;			/* Parent entry */
	TreeServer* Route;			/* Route entry */
	std::vector<TreeServer*> Children;	/* List of child objects */
	std::string ServerName;			/* Server's name */
	std::string ServerDesc;			/* Server's description */
	std::string VersionString;		/* Version string or empty string */
	int UserCount;				/* Not used in this version */
	int OperCount;				/* Not used in this version */
	TreeSocket* Socket;			/* For directly connected servers this points at the socket object */
	time_t NextPing;			/* After this time, the server should be PINGed*/
	bool LastPingWasGood;			/* True if the server responded to the last PING with a PONG */
	
 public:

	/* We don't use this constructor. Its a dummy, and won't cause any insertion
	 * of the TreeServer into the hash_map. See below for the two we DO use.
	 */
	TreeServer()
	{
		Parent = NULL;
		ServerName = "";
		ServerDesc = "";
		VersionString = "";
		UserCount = OperCount = 0;
		VersionString = Srv->GetVersion();
	}

	/* We use this constructor only to create the 'root' item, TreeRoot, which
	 * represents our own server. Therefore, it has no route, no parent, and
	 * no socket associated with it. Its version string is our own local version.
	 */
	TreeServer(std::string Name, std::string Desc) : ServerName(Name), ServerDesc(Desc)
	{
		Parent = NULL;
		VersionString = "";
		UserCount = OperCount = 0;
		VersionString = Srv->GetVersion();
		Route = NULL;
		Socket = NULL; /* Fix by brain */
		AddHashEntry();
	}

	/* When we create a new server, we call this constructor to initialize it.
	 * This constructor initializes the server's Route and Parent, and sets up
	 * its ping counters so that it will be pinged one minute from now.
	 */
	TreeServer(std::string Name, std::string Desc, TreeServer* Above, TreeSocket* Sock) : Parent(Above), ServerName(Name), ServerDesc(Desc), Socket(Sock)
	{
		VersionString = "";
		UserCount = OperCount = 0;
		this->SetNextPingTime(time(NULL) + 300);
		this->SetPingFlag();

		/* find the 'route' for this server (e.g. the one directly connected
		 * to the local server, which we can use to reach it)
		 *
		 * In the following example, consider we have just added a TreeServer
		 * class for server G on our network, of which we are server A.
		 * To route traffic to G (marked with a *) we must send the data to
		 * B (marked with a +) so this algorithm initializes the 'Route'
		 * value to point at whichever server traffic must be routed through
		 * to get here. If we were to try this algorithm with server B,
		 * the Route pointer would point at its own object ('this').
		 *
		 *              A
		 *             / \
		 *          + B   C
		 *           / \   \
		 *          D   E   F
		 *         /         \
		 *      * G           H
		 *
		 * We only run this algorithm when a server is created, as
		 * the routes remain constant while ever the server exists, and
		 * do not need to be re-calculated.
		 */

		Route = Above;
		if (Route == TreeRoot)
		{
			Route = this;
		}
		else
		{
	                while (this->Route->GetParent() != TreeRoot)
			{
				this->Route = Route->GetParent();
			}
		}

		/* Because recursive code is slow and takes a lot of resources,
		 * we store two representations of the server tree. The first
		 * is a recursive structure where each server references its
		 * children and its parent, which is used for netbursts and
		 * netsplits to dump the whole dataset to the other server,
		 * and the second is used for very fast lookups when routing
		 * messages and is instead a hash_map, where each item can
		 * be referenced by its server name. The AddHashEntry()
		 * call below automatically inserts each TreeServer class
		 * into the hash_map as it is created. There is a similar
		 * maintainance call in the destructor to tidy up deleted
		 * servers.
		 */

		this->AddHashEntry();
	}

	/* This method is used to add the structure to the
	 * hash_map for linear searches. It is only called
	 * by the constructors.
	 */
	void AddHashEntry()
	{
		server_hash::iterator iter;
		iter = serverlist.find(this->ServerName);
		if (iter == serverlist.end())
			serverlist[this->ServerName] = this;
	}

	/* This method removes the reference to this object
	 * from the hash_map which is used for linear searches.
	 * It is only called by the default destructor.
	 */
	void DelHashEntry()
	{
		server_hash::iterator iter;
		iter = serverlist.find(this->ServerName);
		if (iter != serverlist.end())
			serverlist.erase(iter);
	}

	/* These accessors etc should be pretty self-
	 * explanitory.
	 */

	TreeServer* GetRoute()
	{
		return Route;
	}

	std::string GetName()
	{
		return ServerName;
	}

	std::string GetDesc()
	{
		return ServerDesc;
	}

	std::string GetVersion()
	{
		return VersionString;
	}

	void SetNextPingTime(time_t t)
	{
		this->NextPing = t;
		LastPingWasGood = false;
	}

	time_t NextPingTime()
	{
		return NextPing;
	}

	bool AnsweredLastPing()
	{
		return LastPingWasGood;
	}

	void SetPingFlag()
	{
		LastPingWasGood = true;
	}

	int GetUserCount()
	{
		return UserCount;
	}

	void AddUserCount()
	{
		UserCount++;
	}

	void DelUserCount()
	{
		UserCount--;
	}

	int GetOperCount()
	{
		return OperCount;
	}

	TreeSocket* GetSocket()
	{
		return Socket;
	}

	TreeServer* GetParent()
	{
		return Parent;
	}

	void SetVersion(std::string Version)
	{
		VersionString = Version;
	}

	unsigned int ChildCount()
	{
		return Children.size();
	}

	TreeServer* GetChild(unsigned int n)
	{
		if (n < Children.size())
		{
			/* Make sure they  cant request
			 * an out-of-range object. After
			 * all we know what these programmer
			 * types are like *grin*.
			 */
			return Children[n];
		}
		else
		{
			return NULL;
		}
	}

	void AddChild(TreeServer* Child)
	{
		Children.push_back(Child);
	}

	bool DelChild(TreeServer* Child)
	{
		for (std::vector<TreeServer*>::iterator a = Children.begin(); a < Children.end(); a++)
		{
			if (*a == Child)
			{
				Children.erase(a);
				return true;
			}
		}
		return false;
	}

	/* Removes child nodes of this node, and of that node, etc etc.
	 * This is used during netsplits to automatically tidy up the
	 * server tree. It is slow, we don't use it for much else.
	 */
	bool Tidy()
	{
		bool stillchildren = true;
		while (stillchildren)
		{
			stillchildren = false;
			for (std::vector<TreeServer*>::iterator a = Children.begin(); a < Children.end(); a++)
			{
				TreeServer* s = (TreeServer*)*a;
				s->Tidy();
				Children.erase(a);
				delete s;
				stillchildren = true;
				break;
			}
		}
		return true;
	}

	~TreeServer()
	{
		/* We'd better tidy up after ourselves, eh? */
		this->DelHashEntry();
	}
};

/* The Link class might as well be a struct,
 * but this is C++ and we don't believe in structs (!).
 * It holds the entire information of one <link>
 * tag from the main config file. We maintain a list
 * of them, and populate the list on rehash/load.
 */

class Link
{
 public:
	 std::string Name;
	 std::string IPAddr;
	 int Port;
	 std::string SendPass;
	 std::string RecvPass;
	 unsigned long AutoConnect;
	 time_t NextConnectTime;
	 std::string EncryptionKey;
};

/* The usual stuff for inspircd modules,
 * plus the vector of Link classes which we
 * use to store the <link> tags from the config
 * file.
 */
ConfigReader *Conf;
std::vector<Link> LinkBlocks;

/* Yay for fast searches!
 * This is hundreds of times faster than recursion
 * or even scanning a linked list, especially when
 * there are more than a few servers to deal with.
 * (read as: lots).
 */
TreeServer* FindServer(std::string ServerName)
{
	server_hash::iterator iter;
	iter = serverlist.find(ServerName);
	if (iter != serverlist.end())
	{
		return iter->second;
	}
	else
	{
		return NULL;
	}
}

/* Returns the locally connected server we must route a
 * message through to reach server 'ServerName'. This
 * only applies to one-to-one and not one-to-many routing.
 * See the comments for the constructor of TreeServer
 * for more details.
 */
TreeServer* BestRouteTo(std::string ServerName)
{
	if (ServerName.c_str() == TreeRoot->GetName())
		return NULL;
	TreeServer* Found = FindServer(ServerName);
	if (Found)
	{
		return Found->GetRoute();
	}
	else
	{
		return NULL;
	}
}

/* Find the first server matching a given glob mask.
 * Theres no find-using-glob method of hash_map [awwww :-(]
 * so instead, we iterate over the list using an iterator
 * and match each one until we get a hit. Yes its slow,
 * deal with it.
 */
TreeServer* FindServerMask(std::string ServerName)
{
	for (server_hash::iterator i = serverlist.begin(); i != serverlist.end(); i++)
	{
		if (Srv->MatchText(i->first,ServerName))
			return i->second;
	}
	return NULL;
}

/* A convenient wrapper that returns true if a server exists */
bool IsServer(std::string ServerName)
{
	return (FindServer(ServerName) != NULL);
}

/* Every SERVER connection inbound or outbound is represented by
 * an object of type TreeSocket.
 * TreeSockets, being inherited from InspSocket, can be tied into
 * the core socket engine, and we cn therefore receive activity events
 * for them, just like activex objects on speed. (yes really, that
 * is a technical term!) Each of these which relates to a locally
 * connected server is assocated with it, by hooking it onto a
 * TreeSocket class using its constructor. In this way, we can
 * maintain a list of servers, some of which are directly connected,
 * some of which are not.
 */

class TreeSocket : public InspSocket
{
	std::string myhost;
	std::string in_buffer;
	ServerState LinkState;
	std::string InboundServerName;
	std::string InboundDescription;
	int num_lost_users;
	int num_lost_servers;
	time_t NextPing;
	bool LastPingWasGood;
	bool bursting;
	AES* ctx_in;
	AES* ctx_out;
	unsigned int keylength;
	
 public:

	/* Because most of the I/O gubbins are encapsulated within
	 * InspSocket, we just call the superclass constructor for
	 * most of the action, and append a few of our own values
	 * to it.
	 */
	TreeSocket(std::string host, int port, bool listening, unsigned long maxtime)
		: InspSocket(host, port, listening, maxtime)
	{
		myhost = host;
		this->LinkState = LISTENER;
		this->ctx_in = NULL;
		this->ctx_out = NULL;
	}

	TreeSocket(std::string host, int port, bool listening, unsigned long maxtime, std::string ServerName)
		: InspSocket(host, port, listening, maxtime)
	{
		myhost = ServerName;
		this->LinkState = CONNECTING;
		this->ctx_in = NULL;
		this->ctx_out = NULL;
	}

	/* When a listening socket gives us a new file descriptor,
	 * we must associate it with a socket without creating a new
	 * connection. This constructor is used for this purpose.
	 */
	TreeSocket(int newfd, char* ip)
		: InspSocket(newfd, ip)
	{
		this->LinkState = WAIT_AUTH_1;
		this->ctx_in = NULL;
		this->ctx_out = NULL;
		this->SendCapabilities();
	}

	~TreeSocket()
	{
		if (ctx_in)
			delete ctx_in;
		if (ctx_out)
			delete ctx_out;
	}

	void InitAES(std::string key,std::string SName)
	{
		if (key == "")
			return;

		ctx_in = new AES();
		ctx_out = new AES();
		log(DEBUG,"Initialized AES key %s",key.c_str());
		// key must be 16, 24, 32 etc bytes (multiple of 8)
		keylength = key.length();
		if (!(keylength == 16 || keylength == 24 || keylength == 32))
		{
			WriteOpers("*** \2ERROR\2: Key length for encryptionkey is not 16, 24 or 32 bytes in length!");
			log(DEBUG,"Key length not 16, 24 or 32 characters!");
		}
		else
		{
			WriteOpers("*** \2AES\2: Initialized %d bit encryption to server %s",keylength*8,SName.c_str());
			ctx_in->MakeKey(key.c_str(), "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\
				\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", keylength, keylength);
			ctx_out->MakeKey(key.c_str(), "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\
				\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", keylength, keylength);
		}
	}
	
	/* When an outbound connection finishes connecting, we receive
	 * this event, and must send our SERVER string to the other
	 * side. If the other side is happy, as outlined in the server
	 * to server docs on the inspircd.org site, the other side
	 * will then send back its own server string.
	 */
        virtual bool OnConnected()
	{
		if (this->LinkState == CONNECTING)
		{
			Srv->SendOpers("*** Connection to "+myhost+"["+this->GetIP()+"] established.");
			/* we do not need to change state here. */
			for (std::vector<Link>::iterator x = LinkBlocks.begin(); x < LinkBlocks.end(); x++)
			{
				if (x->Name == this->myhost)
				{
					this->SendCapabilities();
					if (x->EncryptionKey != "")
					{
						if (!(x->EncryptionKey.length() == 16 || x->EncryptionKey.length() == 24 || x->EncryptionKey.length() == 32))
						{
							WriteOpers("\2WARNING\2: Your encryption key is NOT 16, 24 or 32 characters in length, encryption will \2NOT\2 be enabled.");
						}
						else
						{
							this->WriteLine("AES "+Srv->GetServerName());
							this->InitAES(x->EncryptionKey,x->Name);
						}
					}
					/* found who we're supposed to be connecting to, send the neccessary gubbins. */
					this->WriteLine("SERVER "+Srv->GetServerName()+" "+x->SendPass+" 0 :"+Srv->GetServerDescription());
					return true;
				}
			}
		}
		/* There is a (remote) chance that between the /CONNECT and the connection
		 * being accepted, some muppet has removed the <link> block and rehashed.
		 * If that happens the connection hangs here until it's closed. Unlikely
		 * and rather harmless.
		 */
		return true;
	}
	
        virtual void OnError(InspSocketError e)
	{
		/* We don't handle this method, because all our
		 * dirty work is done in OnClose() (see below)
		 * which is still called on error conditions too.
		 */
	}

        virtual int OnDisconnect()
	{
		/* For the same reason as above, we don't
		 * handle OnDisconnect()
		 */
		return true;
	}

	/* Recursively send the server tree with distances as hops.
	 * This is used during network burst to inform the other server
	 * (and any of ITS servers too) of what servers we know about.
	 * If at any point any of these servers already exist on the other
	 * end, our connection may be terminated. The hopcounts given
	 * by this function are relative, this doesn't matter so long as
	 * they are all >1, as all the remote servers re-calculate them
	 * to be relative too, with themselves as hop 0.
	 */
	void SendServers(TreeServer* Current, TreeServer* s, int hops)
	{
		char command[1024];
		for (unsigned int q = 0; q < Current->ChildCount(); q++)
		{
			TreeServer* recursive_server = Current->GetChild(q);
			if (recursive_server != s)
			{
				snprintf(command,1024,":%s SERVER %s * %d :%s",Current->GetName().c_str(),recursive_server->GetName().c_str(),hops,recursive_server->GetDesc().c_str());
				this->WriteLine(command);
				this->WriteLine(":"+recursive_server->GetName()+" VERSION :"+recursive_server->GetVersion());
				/* down to next level */
				this->SendServers(recursive_server, s, hops+1);
			}
		}
	}

	std::string MyCapabilities()
	{
		ServerConfig* Config = Srv->GetConfig();
		std::vector<std::string> modlist;
		std::string capabilities = "";

                for (int i = 0; i <= MODCOUNT; i++)
                {
			if ((modules[i]->GetVersion().Flags & VF_STATIC) || (modules[i]->GetVersion().Flags & VF_COMMON))
				modlist.push_back(Config->module_names[i]);
                }
		sort(modlist.begin(),modlist.end());
		for (unsigned int i = 0; i < modlist.size(); i++)
		{
			if (i)
				capabilities = capabilities + ",";
			capabilities = capabilities + modlist[i];
		}
		return capabilities;
	}
	
	void SendCapabilities()
	{
		this->WriteLine("CAPAB "+MyCapabilities());
	}

	bool Capab(std::deque<std::string> params)
	{
		if (params.size() != 1)
		{
			this->WriteLine("ERROR :Invalid number of parameters for CAPAB");
			return false;
		}
		if (params[0] != this->MyCapabilities())
		{
	                std::string quitserver = this->myhost;
	                if (this->InboundServerName != "")
	                {
	                        quitserver = this->InboundServerName;
	                }
			WriteOpers("*** \2ERROR\2: Server '%s' does not have the same set of modules loaded, cannot link!",quitserver.c_str());
			WriteOpers("*** Our networked module set is: '%s'",this->MyCapabilities().c_str());
			WriteOpers("*** Other server's networked module set is: '%s'",params[0].c_str());
			WriteOpers("*** These lists must match exactly on both servers. Please correct these errors, and try again.");
			this->WriteLine("ERROR :CAPAB mismatch; My capabilities: '"+this->MyCapabilities()+"'");
			return false;
		}
		return true;
	}

	/* This function forces this server to quit, removing this server
	 * and any users on it (and servers and users below that, etc etc).
	 * It's very slow and pretty clunky, but luckily unless your network
	 * is having a REAL bad hair day, this function shouldnt be called
	 * too many times a month ;-)
	 */
	void SquitServer(TreeServer* Current, CullList* Goners)
	{
		/* recursively squit the servers attached to 'Current'.
		 * We're going backwards so we don't remove users
		 * while we still need them ;)
		 */
		for (unsigned int q = 0; q < Current->ChildCount(); q++)
		{
			TreeServer* recursive_server = Current->GetChild(q);
			this->SquitServer(recursive_server,Goners);
		}
		/* Now we've whacked the kids, whack self */
		num_lost_servers++;
		for (user_hash::iterator u = clientlist.begin(); u != clientlist.end(); u++)
		{
			if (!strcasecmp(u->second->server,Current->GetName().c_str()))
			{
				std::string qreason = Current->GetName()+" "+std::string(Srv->GetServerName());
				Goners->AddItem(u->second,qreason);
				num_lost_users++;
			}
		}
	}

	/* This is a wrapper function for SquitServer above, which
	 * does some validation first and passes on the SQUIT to all
	 * other remaining servers.
	 */
	void Squit(TreeServer* Current,std::string reason)
	{
		if ((Current) && (Current != TreeRoot))
		{
			std::deque<std::string> params;
			params.push_back(Current->GetName());
			params.push_back(":"+reason);
			DoOneToAllButSender(Current->GetParent()->GetName(),"SQUIT",params,Current->GetName());
			if (Current->GetParent() == TreeRoot)
			{
				Srv->SendOpers("Server \002"+Current->GetName()+"\002 split: "+reason);
			}
			else
			{
				Srv->SendOpers("Server \002"+Current->GetName()+"\002 split from server \002"+Current->GetParent()->GetName()+"\002 with reason: "+reason);
			}
			num_lost_servers = 0;
			num_lost_users = 0;
			CullList* Goners = new CullList();
			SquitServer(Current, Goners);
			Goners->Apply();
			Current->Tidy();
			Current->GetParent()->DelChild(Current);
			delete Current;
			delete Goners;
			WriteOpers("Netsplit complete, lost \002%d\002 users on \002%d\002 servers.", num_lost_users, num_lost_servers);
		}
		else
		{
			log(DEFAULT,"Squit from unknown server");
		}
	}

	/* FMODE command */
	bool ForceMode(std::string source, std::deque<std::string> params)
	{
		userrec* who = new userrec;
		who->fd = FD_MAGIC_NUMBER;
		if (params.size() < 2)
			return true;
		char* modelist[255];
		for (unsigned int q = 0; q < params.size(); q++)
		{
			modelist[q] = (char*)params[q].c_str();
		}
		Srv->SendMode(modelist,params.size(),who);
		DoOneToAllButSender(source,"FMODE",params,source);
		delete who;
		return true;
	}

	/* FTOPIC command */
	bool ForceTopic(std::string source, std::deque<std::string> params)
	{
		if (params.size() != 4)
			return true;
		std::string channel = params[0];
		time_t ts = atoi(params[1].c_str());
		std::string setby = params[2];
		std::string topic = params[3];

		chanrec* c = Srv->FindChannel(channel);
		if (c)
		{
			if ((ts >= c->topicset) || (!*c->topic))
			{
				std::string oldtopic = c->topic;
				strlcpy(c->topic,topic.c_str(),MAXTOPIC);
				strlcpy(c->setby,setby.c_str(),NICKMAX);
				c->topicset = ts;
				/* if the topic text is the same as the current topic,
				 * dont bother to send the TOPIC command out, just silently
				 * update the set time and set nick.
				 */
				if (oldtopic != topic)
					WriteChannelWithServ((char*)source.c_str(), c, "TOPIC %s :%s", c->name, c->topic);
			}
			
		}
		
		/* all done, send it on its way */
		params[3] = ":" + params[3];
		DoOneToAllButSender(source,"FTOPIC",params,source);

		return true;
	}

	/* FJOIN, similar to unreal SJOIN */
	bool ForceJoin(std::string source, std::deque<std::string> params)
	{
		if (params.size() < 3)
			return true;

		char first[MAXBUF];
		char modestring[MAXBUF];
		char* mode_users[127];
		mode_users[0] = first;
		mode_users[1] = modestring;
		strcpy(mode_users[1],"+");
		unsigned int modectr = 2;
		
		userrec* who = NULL;
		std::string channel = params[0];
		time_t TS = atoi(params[1].c_str());
		char* key = "";
		
		chanrec* chan = Srv->FindChannel(channel);
		if (chan)
		{
			key = chan->key;
		}
		strlcpy(mode_users[0],channel.c_str(),MAXBUF);

		/* default is a high value, which if we dont have this
		 * channel will let the other side apply their modes.
		 */
		time_t ourTS = time(NULL)+600;
		chanrec* us = Srv->FindChannel(channel);
		if (us)
		{
			ourTS = us->age;
		}

		log(DEBUG,"FJOIN detected, our TS=%lu, their TS=%lu",ourTS,TS);

		/* do this first, so our mode reversals are correctly received by other servers
		 * if there is a TS collision.
		 */
		DoOneToAllButSender(source,"FJOIN",params,source);
		
		for (unsigned int usernum = 2; usernum < params.size(); usernum++)
		{
			/* process one channel at a time, applying modes. */
			char* usr = (char*)params[usernum].c_str();
			char permissions = *usr;
			switch (permissions)
			{
				case '@':
					usr++;
					mode_users[modectr++] = usr;
					strlcat(modestring,"o",MAXBUF);
				break;
				case '%':
					usr++;
					mode_users[modectr++] = usr;
					strlcat(modestring,"h",MAXBUF);
				break;
				case '+':
					usr++;
					mode_users[modectr++] = usr;
					strlcat(modestring,"v",MAXBUF);
				break;
			}
			who = Srv->FindNick(usr);
			if (who)
			{
				Srv->JoinUserToChannel(who,channel,key);
				if (modectr >= (MAXMODES-1))
				{
					/* theres a mode for this user. push them onto the mode queue, and flush it
					 * if there are more than MAXMODES to go.
					 */
					if ((ourTS >= TS) || (Srv->IsUlined(who->server)))
					{
						/* We also always let u-lined clients win, no matter what the TS value */
						log(DEBUG,"Our our channel newer than theirs, accepting their modes");
						Srv->SendMode(mode_users,modectr,who);
					}
					else
					{
						log(DEBUG,"Their channel newer than ours, bouncing their modes");
						/* bouncy bouncy! */
						std::deque<std::string> params;
						/* modes are now being UNSET... */
						*mode_users[1] = '-';
						for (unsigned int x = 0; x < modectr; x++)
						{
							params.push_back(mode_users[x]);
						}
						// tell everyone to bounce the modes. bad modes, bad!
						DoOneToMany(Srv->GetServerName(),"FMODE",params);
					}
					strcpy(mode_users[1],"+");
					modectr = 2;
				}
			}
		}
		/* there werent enough modes built up to flush it during FJOIN,
		 * or, there are a number left over. flush them out.
		 */
		if ((modectr > 2) && (who))
		{
			if (ourTS >= TS)
			{
				log(DEBUG,"Our our channel newer than theirs, accepting their modes");
				Srv->SendMode(mode_users,modectr,who);
			}
			else
			{
				log(DEBUG,"Their channel newer than ours, bouncing their modes");
				std::deque<std::string> params;
				*mode_users[1] = '-';
				for (unsigned int x = 0; x < modectr; x++)
				{
					params.push_back(mode_users[x]);
				}
				DoOneToMany(Srv->GetServerName(),"FMODE",params);
			}
		}
		return true;
	}

	/* NICK command */
	bool IntroduceClient(std::string source, std::deque<std::string> params)
	{
		if (params.size() < 8)
			return true;
		// NICK age nick host dhost ident +modes ip :gecos
		//       0   1    2    3      4     5    6   7
		std::string nick = params[1];
		std::string host = params[2];
		std::string dhost = params[3];
		std::string ident = params[4];
		time_t age = atoi(params[0].c_str());
		std::string modes = params[5];
		while (*(modes.c_str()) == '+')
		{
			char* m = (char*)modes.c_str();
			m++;
			modes = m;
		}
		std::string ip = params[6];
		std::string gecos = params[7];
		char* tempnick = (char*)nick.c_str();
		log(DEBUG,"Introduce client %s!%s@%s",tempnick,ident.c_str(),host.c_str());
		
		user_hash::iterator iter;
		iter = clientlist.find(tempnick);
		if (iter != clientlist.end())
		{
			// nick collision
			log(DEBUG,"Nick collision on %s!%s@%s: %lu %lu",tempnick,ident.c_str(),host.c_str(),(unsigned long)age,(unsigned long)iter->second->age);
			this->WriteLine(":"+Srv->GetServerName()+" KILL "+tempnick+" :Nickname collision");
			return true;
		}

		clientlist[tempnick] = new userrec();
		clientlist[tempnick]->fd = FD_MAGIC_NUMBER;
		strlcpy(clientlist[tempnick]->nick, tempnick,NICKMAX);
		strlcpy(clientlist[tempnick]->host, host.c_str(),160);
		strlcpy(clientlist[tempnick]->dhost, dhost.c_str(),160);
		clientlist[tempnick]->server = (char*)FindServerNamePtr(source.c_str());
		strlcpy(clientlist[tempnick]->ident, ident.c_str(),IDENTMAX);
		strlcpy(clientlist[tempnick]->fullname, gecos.c_str(),MAXGECOS);
		clientlist[tempnick]->registered = 7;
		clientlist[tempnick]->signon = age;
		strlcpy(clientlist[tempnick]->modes, modes.c_str(),53);
		strlcpy(clientlist[tempnick]->ip,ip.c_str(),16);

		ucrec a;
		a.channel = NULL;
		a.uc_modes = 0;
		for (int i = 0; i < MAXCHANS; i++)
			clientlist[tempnick]->chans.push_back(a);

		if (!this->bursting)
		{
			WriteOpers("*** Client connecting at %s: %s!%s@%s [%s]",clientlist[tempnick]->server,clientlist[tempnick]->nick,clientlist[tempnick]->ident,clientlist[tempnick]->host,clientlist[tempnick]->ip);
		}
		params[7] = ":" + params[7];
		DoOneToAllButSender(source,"NICK",params,source);

		// Increment the Source Servers User Count..
		TreeServer* SourceServer = FindServer(source);
		if (SourceServer) {
			SourceServer->AddUserCount();
		}

		return true;
	}

	/* Send one or more FJOINs for a channel of users.
	 * If the length of a single line is more than 480-NICKMAX
	 * in length, it is split over multiple lines.
	 */
	void SendFJoins(TreeServer* Current, chanrec* c)
	{
		log(DEBUG,"Sending FJOINs to other server for %s",c->name);
		char list[MAXBUF];
		snprintf(list,MAXBUF,":%s FJOIN %s %lu",Srv->GetServerName().c_str(),c->name,(unsigned long)c->age);
		std::vector<char*> *ulist = c->GetUsers();
		for (unsigned int i = 0; i < ulist->size(); i++)
		{
			char* o = (*ulist)[i];
			userrec* otheruser = (userrec*)o;
			strlcat(list," ",MAXBUF);
			strlcat(list,cmode(otheruser,c),MAXBUF);
			strlcat(list,otheruser->nick,MAXBUF);
			if (strlen(list)>(480-NICKMAX))
			{
				log(DEBUG,"FJOIN line wrapped");
				this->WriteLine(list);
				snprintf(list,MAXBUF,":%s FJOIN %s %lu",Srv->GetServerName().c_str(),c->name,(unsigned long)c->age);
			}
		}
		if (list[strlen(list)-1] != ':')
		{
			log(DEBUG,"Final FJOIN line");
			this->WriteLine(list);
		}
	}

	/* Send G, Q, Z and E lines */
	void SendXLines(TreeServer* Current)
	{
		char data[MAXBUF];
		/* Yes, these arent too nice looking, but they get the job done */
		for (std::vector<ZLine>::iterator i = zlines.begin(); i != zlines.end(); i++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE Z %s %s %lu %lu :%s",Srv->GetServerName().c_str(),i->ipaddr,i->source,(unsigned long)i->set_time,(unsigned long)i->duration,i->reason);
			this->WriteLine(data);
		}
		for (std::vector<QLine>::iterator i = qlines.begin(); i != qlines.end(); i++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE Q %s %s %lu %lu :%s",Srv->GetServerName().c_str(),i->nick,i->source,(unsigned long)i->set_time,(unsigned long)i->duration,i->reason);
			this->WriteLine(data);
		}
		for (std::vector<GLine>::iterator i = glines.begin(); i != glines.end(); i++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE G %s %s %lu %lu :%s",Srv->GetServerName().c_str(),i->hostmask,i->source,(unsigned long)i->set_time,(unsigned long)i->duration,i->reason);
			this->WriteLine(data);
		}
		for (std::vector<ELine>::iterator i = elines.begin(); i != elines.end(); i++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE E %s %s %lu %lu :%s",Srv->GetServerName().c_str(),i->hostmask,i->source,(unsigned long)i->set_time,(unsigned long)i->duration,i->reason);
			this->WriteLine(data);
		}
		for (std::vector<ZLine>::iterator i = pzlines.begin(); i != pzlines.end(); i++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE Z %s %s %lu %lu :%s",Srv->GetServerName().c_str(),i->ipaddr,i->source,(unsigned long)i->set_time,(unsigned long)i->duration,i->reason);
			this->WriteLine(data);
		}
		for (std::vector<QLine>::iterator i = pqlines.begin(); i != pqlines.end(); i++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE Q %s %s %lu %lu :%s",Srv->GetServerName().c_str(),i->nick,i->source,(unsigned long)i->set_time,(unsigned long)i->duration,i->reason);
			this->WriteLine(data);
		}
		for (std::vector<GLine>::iterator i = pglines.begin(); i != pglines.end(); i++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE G %s %s %lu %lu :%s",Srv->GetServerName().c_str(),i->hostmask,i->source,(unsigned long)i->set_time,(unsigned long)i->duration,i->reason);
			this->WriteLine(data);
		}
		for (std::vector<ELine>::iterator i = pelines.begin(); i != pelines.end(); i++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE E %s %s %lu %lu :%s",Srv->GetServerName().c_str(),i->hostmask,i->source,(unsigned long)i->set_time,(unsigned long)i->duration,i->reason);
			this->WriteLine(data);
		}
	}

	/* Send channel modes and topics */
	void SendChannelModes(TreeServer* Current)
	{
		char data[MAXBUF];
		std::deque<std::string> list;
		for (chan_hash::iterator c = chanlist.begin(); c != chanlist.end(); c++)
		{
			SendFJoins(Current, c->second);
			snprintf(data,MAXBUF,":%s FMODE %s +%s",Srv->GetServerName().c_str(),c->second->name,chanmodes(c->second));
			this->WriteLine(data);
			if (*c->second->topic)
			{
				snprintf(data,MAXBUF,":%s FTOPIC %s %lu %s :%s",Srv->GetServerName().c_str(),c->second->name,(unsigned long)c->second->topicset,c->second->setby,c->second->topic);
				this->WriteLine(data);
			}
			for (BanList::iterator b = c->second->bans.begin(); b != c->second->bans.end(); b++)
			{
				snprintf(data,MAXBUF,":%s FMODE %s +b %s",Srv->GetServerName().c_str(),c->second->name,b->data);
				this->WriteLine(data);
			}
			FOREACH_MOD(I_OnSyncChannel,OnSyncChannel(c->second,(Module*)TreeProtocolModule,(void*)this));
			list.clear();
			c->second->GetExtList(list);
			for (unsigned int j = 0; j < list.size(); j++)
			{
				FOREACH_MOD(I_OnSyncChannelMetaData,OnSyncChannelMetaData(c->second,(Module*)TreeProtocolModule,(void*)this,list[j]));
			}
		}
	}

	/* send all users and their oper state/modes */
	void SendUsers(TreeServer* Current)
	{
		char data[MAXBUF];
		std::deque<std::string> list;
		for (user_hash::iterator u = clientlist.begin(); u != clientlist.end(); u++)
		{
			if (u->second->registered == 7)
			{
				snprintf(data,MAXBUF,":%s NICK %lu %s %s %s %s +%s %s :%s",u->second->server,(unsigned long)u->second->age,u->second->nick,u->second->host,u->second->dhost,u->second->ident,u->second->modes,u->second->ip,u->second->fullname);
				this->WriteLine(data);
				if (strchr(u->second->modes,'o'))
				{
					this->WriteLine(":"+std::string(u->second->nick)+" OPERTYPE "+std::string(u->second->oper));
				}
				FOREACH_MOD(I_OnSyncUser,OnSyncUser(u->second,(Module*)TreeProtocolModule,(void*)this));
				list.clear();
				u->second->GetExtList(list);
				for (unsigned int j = 0; j < list.size(); j++)
				{
					FOREACH_MOD(I_OnSyncUserMetaData,OnSyncUserMetaData(u->second,(Module*)TreeProtocolModule,(void*)this,list[j]));
				}
			}
		}
	}

	/* This function is called when we want to send a netburst to a local
	 * server. There is a set order we must do this, because for example
	 * users require their servers to exist, and channels require their
	 * users to exist. You get the idea.
	 */
	void DoBurst(TreeServer* s)
	{
		Srv->SendOpers("*** Bursting to \2"+s->GetName()+"\2.");
		this->WriteLine("BURST");
		/* send our version string */
		this->WriteLine(":"+Srv->GetServerName()+" VERSION :"+Srv->GetVersion());
		/* Send server tree */
		this->SendServers(TreeRoot,s,1);
		/* Send users and their oper status */
		this->SendUsers(s);
		/* Send everything else (channel modes, xlines etc) */
		this->SendChannelModes(s);
		this->SendXLines(s);
		this->WriteLine("ENDBURST");
		Srv->SendOpers("*** Finished bursting to \2"+s->GetName()+"\2.");
	}

	/* This function is called when we receive data from a remote
	 * server. We buffer the data in a std::string (it doesnt stay
	 * there for long), reading using InspSocket::Read() which can
	 * read up to 16 kilobytes in one operation.
	 *
	 * IF THIS FUNCTION RETURNS FALSE, THE CORE CLOSES AND DELETES
	 * THE SOCKET OBJECT FOR US.
	 */
        virtual bool OnDataReady()
	{
		char* data = this->Read();
		if (data)
		{
			this->in_buffer += data;
			/* While there is at least one new line in the buffer,
			 * do something useful (we hope!) with it.
			 */
			while (in_buffer.find("\n") != std::string::npos)
			{
				char* line = (char*)in_buffer.c_str();
				std::string ret = "";
			        while ((*line != '\n') && (strlen(line)))
				{
					if ((*line != '\r') && (*line != '\n'))
						ret = ret + *line;
					line++;
				}
				if ((*line == '\n') || (*line == '\r'))
					line++;
				in_buffer = line;
				/* Process this one, abort if it
				 * didnt return true.
				 */
				if (this->ctx_in)
				{
					char out[1024];
					char result[1024];
					memset(result,0,1024);
					memset(out,0,1024);
					log(DEBUG,"Original string '%s'",ret.c_str());
					/* ERROR + CAPAB is still allowed unencryped */
					if ((ret.substr(0,7) != "ERROR :") && (ret.substr(0,6) != "CAPAB "))
					{
						int nbytes = from64tobits(out, ret.c_str(), 1024);
						if ((nbytes > 0) && (nbytes < 1024))
						{
							log(DEBUG,"m_spanningtree: decrypt %d bytes",nbytes);
							ctx_in->Decrypt(out, result, nbytes, 1);
							for (int t = 0; t < nbytes; t++)
								if (result[t] == '\7') result[t] = 0;
							ret = result;
						}
					}
				}
				if (!this->ProcessLine(ret))
				{
					return false;
				}
			}
		}
		return (data != NULL);
	}

	int WriteLine(std::string line)
	{
		log(DEBUG,"OUT: %s",line.c_str());
		if (this->ctx_out)
		{
			log(DEBUG,"AES context");
			char result[10240];
			char result64[10240];
			if (this->keylength)
			{
				while (line.length() % this->keylength != 0)
				{
					// pad it to be a multiple of the key length
					line = line + "\7";
				}
			}
			unsigned int ll = line.length();
			log(DEBUG,"Plaintext line with padding = %d chars",ll);
			ctx_out->Encrypt(line.c_str(), result, ll, 1);
			log(DEBUG,"Encrypted.");
			to64frombits((unsigned char*)result64,(unsigned char*)result,ll);
			line = result64;
			log(DEBUG,"Encrypted: %s",line.c_str());
			//int from64tobits(char *out, const char *in, int maxlen);
		}
		return this->Write(line + "\r\n");
	}

	/* Handle ERROR command */
	bool Error(std::deque<std::string> params)
	{
		if (params.size() < 1)
			return false;
		std::string Errmsg = params[0];
		std::string SName = myhost;
		if (InboundServerName != "")
		{
			SName = InboundServerName;
		}
		Srv->SendOpers("*** ERROR from "+SName+": "+Errmsg);
		/* we will return false to cause the socket to close.
		 */
		return false;
	}

	/* Because the core won't let users or even SERVERS set +o,
	 * we use the OPERTYPE command to do this.
	 */
	bool OperType(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() != 1)
			return true;
		std::string opertype = params[0];
		userrec* u = Srv->FindNick(prefix);
		if (u)
		{
			strlcpy(u->oper,opertype.c_str(),NICKMAX);
			if (!strchr(u->modes,'o'))
			{
				strcat(u->modes,"o");
			}
			DoOneToAllButSender(u->nick,"OPERTYPE",params,u->server);
		}
		return true;
	}

	/* Because Andy insists that services-compatible servers must
	 * implement SVSNICK and SVSJOIN, that's exactly what we do :p
	 */
	bool ForceNick(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 3)
			return true;
		userrec* u = Srv->FindNick(params[0]);
		if (u)
		{
			Srv->ChangeUserNick(u,params[1]);
			u->age = atoi(params[2].c_str());
			DoOneToAllButSender(prefix,"SVSNICK",params,prefix);
		}
		return true;
	}

	bool ServiceJoin(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 2)
			return true;
		userrec* u = Srv->FindNick(params[0]);
		if (u)
		{
			Srv->JoinUserToChannel(u,params[1],"");
			DoOneToAllButSender(prefix,"SVSJOIN",params,prefix);
		}
		return true;
	}

	bool RemoteRehash(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return false;
		std::string servermask = params[0];
		if (Srv->MatchText(Srv->GetServerName(),servermask))
		{
			Srv->SendOpers("*** Remote rehash initiated from server \002"+prefix+"\002.");
			Srv->RehashServer();
			ReadConfiguration(false);
		}
		DoOneToAllButSender(prefix,"REHASH",params,prefix);
		return true;
	}

	bool RemoteKill(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() != 2)
			return true;
		std::string nick = params[0];
		userrec* u = Srv->FindNick(prefix);
		userrec* who = Srv->FindNick(nick);
		if (who)
		{
			/* Prepend kill source, if we don't have one */
			std::string sourceserv = prefix;
			if (u)
			{
				sourceserv = u->server;
			}
			if (*(params[1].c_str()) != '[')
			{
				params[1] = "[" + sourceserv + "] Killed (" + params[1] +")";
			}
			std::string reason = params[1];
			params[1] = ":" + params[1];
			DoOneToAllButSender(prefix,"KILL",params,sourceserv);
			Srv->QuitUser(who,reason);
		}
		return true;
	}

	bool LocalPong(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;
		TreeServer* ServerSource = FindServer(prefix);
		if (ServerSource)
		{
			ServerSource->SetPingFlag();
		}
		return true;
	}
	
	bool MetaData(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 3)
			return true;
		TreeServer* ServerSource = FindServer(prefix);
		if (ServerSource)
		{
			if (*(params[0].c_str()) == '#')
			{
				chanrec* c = Srv->FindChannel(params[0]);
				if (c)
				{
					FOREACH_MOD(I_OnDecodeMetaData,OnDecodeMetaData(TYPE_CHANNEL,c,params[1],params[2]));
				}
			}
			else
			{
				userrec* u = Srv->FindNick(params[0]);
				if (u)
				{
					FOREACH_MOD(I_OnDecodeMetaData,OnDecodeMetaData(TYPE_USER,u,params[1],params[2]));
				}
			}
		}
		params[2] = ":" + params[2];
		DoOneToAllButSender(prefix,"METADATA",params,prefix);
		return true;
	}

	bool ServerVersion(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;
		TreeServer* ServerSource = FindServer(prefix);
		if (ServerSource)
		{
			ServerSource->SetVersion(params[0]);
		}
		params[0] = ":" + params[0];
		DoOneToAllButSender(prefix,"VERSION",params,prefix);
		return true;
	}

	bool ChangeHost(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;
		userrec* u = Srv->FindNick(prefix);
		if (u)
		{
			Srv->ChangeHost(u,params[0]);
			DoOneToAllButSender(prefix,"FHOST",params,u->server);
		}
		return true;
	}

	bool AddLine(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 6)
			return true;
		std::string linetype = params[0]; /* Z, Q, E, G, K */
		std::string mask = params[1]; /* Line type dependent */
		std::string source = params[2]; /* may not be online or may be a server */
		std::string settime = params[3]; /* EPOCH time set */
		std::string duration = params[4]; /* Duration secs */
		std::string reason = params[5];

		switch (*(linetype.c_str()))
		{
			case 'Z':
				add_zline(atoi(duration.c_str()), source.c_str(), reason.c_str(), mask.c_str());
				zline_set_creation_time((char*)mask.c_str(), atoi(settime.c_str()));
			break;
			case 'Q':
				add_qline(atoi(duration.c_str()), source.c_str(), reason.c_str(), mask.c_str());
				qline_set_creation_time((char*)mask.c_str(), atoi(settime.c_str()));
			break;
			case 'E':
				add_eline(atoi(duration.c_str()), source.c_str(), reason.c_str(), mask.c_str());
				eline_set_creation_time((char*)mask.c_str(), atoi(settime.c_str()));
			break;
			case 'G':
				add_gline(atoi(duration.c_str()), source.c_str(), reason.c_str(), mask.c_str());
				gline_set_creation_time((char*)mask.c_str(), atoi(settime.c_str()));
			break;
			case 'K':
				add_kline(atoi(duration.c_str()), source.c_str(), reason.c_str(), mask.c_str());
			break;
			default:
				/* Just in case... */
				Srv->SendOpers("*** \2WARNING\2: Invalid xline type '"+linetype+"' sent by server "+prefix+", ignored!");
			break;
		}
		/* Send it on its way */
		params[5] = ":" + params[5];
		DoOneToAllButSender(prefix,"ADDLINE",params,prefix);
		return true;
	}

	bool ChangeName(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;
		userrec* u = Srv->FindNick(prefix);
		if (u)
		{
			Srv->ChangeGECOS(u,params[0]);
			params[0] = ":" + params[0];
			DoOneToAllButSender(prefix,"FNAME",params,u->server);
		}
		return true;
	}

	bool Whois(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;
		log(DEBUG,"In IDLE command");
		userrec* u = Srv->FindNick(prefix);
		if (u)
		{
			log(DEBUG,"USER EXISTS: %s",u->nick);
			// an incoming request
			if (params.size() == 1)
			{
				userrec* x = Srv->FindNick(params[0]);
				if ((x) && (x->fd > -1))
				{
					userrec* x = Srv->FindNick(params[0]);
					log(DEBUG,"Got IDLE");
					char signon[MAXBUF];
					char idle[MAXBUF];
					log(DEBUG,"Sending back IDLE 3");
					snprintf(signon,MAXBUF,"%lu",(unsigned long)x->signon);
					snprintf(idle,MAXBUF,"%lu",(unsigned long)abs((x->idle_lastmsg)-time(NULL)));
					std::deque<std::string> par;
					par.push_back(prefix);
					par.push_back(signon);
					par.push_back(idle);
					// ours, we're done, pass it BACK
					DoOneToOne(params[0],"IDLE",par,u->server);
				}
				else
				{
					// not ours pass it on
					DoOneToOne(prefix,"IDLE",params,x->server);
				}
			}
			else if (params.size() == 3)
			{
				std::string who_did_the_whois = params[0];
				userrec* who_to_send_to = Srv->FindNick(who_did_the_whois);
				if ((who_to_send_to) && (who_to_send_to->fd > -1))
				{
					log(DEBUG,"Got final IDLE");
					// an incoming reply to a whois we sent out
					std::string nick_whoised = prefix;
					unsigned long signon = atoi(params[1].c_str());
					unsigned long idle = atoi(params[2].c_str());
					if ((who_to_send_to) && (who_to_send_to->fd > -1))
						do_whois(who_to_send_to,u,signon,idle,(char*)nick_whoised.c_str());
				}
				else
				{
					// not ours, pass it on
					DoOneToOne(prefix,"IDLE",params,who_to_send_to->server);
				}
			}
		}
		return true;
	}
	
	bool LocalPing(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;
		std::string stufftobounce = params[0];
		this->WriteLine(":"+Srv->GetServerName()+" PONG "+stufftobounce);
		return true;
	}

	bool RemoteServer(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 4)
			return false;
		std::string servername = params[0];
		std::string password = params[1];
		// hopcount is not used for a remote server, we calculate this ourselves
		std::string description = params[3];
		TreeServer* ParentOfThis = FindServer(prefix);
		if (!ParentOfThis)
		{
			this->WriteLine("ERROR :Protocol error - Introduced remote server from unknown server "+prefix);
			return false;
		}
		TreeServer* CheckDupe = FindServer(servername);
		if (CheckDupe)
		{
			this->WriteLine("ERROR :Server "+servername+" already exists on server "+CheckDupe->GetParent()->GetName()+"!");
			Srv->SendOpers("*** Server connection from \2"+servername+"\2 denied, already exists on server "+CheckDupe->GetParent()->GetName());
			return false;
		}
		TreeServer* Node = new TreeServer(servername,description,ParentOfThis,NULL);
		ParentOfThis->AddChild(Node);
		params[3] = ":" + params[3];
		DoOneToAllButSender(prefix,"SERVER",params,prefix);
		Srv->SendOpers("*** Server \002"+prefix+"\002 introduced server \002"+servername+"\002 ("+description+")");
		return true;
	}

	bool Outbound_Reply_Server(std::deque<std::string> &params)
	{
		if (params.size() < 4)
			return false;
		std::string servername = params[0];
		std::string password = params[1];
		int hops = atoi(params[2].c_str());
		if (hops)
		{
			this->WriteLine("ERROR :Server too far away for authentication");
			Srv->SendOpers("*** Server connection from \2"+servername+"\2 denied, server is too far away for authentication");
			return false;
		}
		std::string description = params[3];
		for (std::vector<Link>::iterator x = LinkBlocks.begin(); x < LinkBlocks.end(); x++)
		{
			if ((x->Name == servername) && (x->RecvPass == password))
			{
				TreeServer* CheckDupe = FindServer(servername);
				if (CheckDupe)
				{
					this->WriteLine("ERROR :Server "+servername+" already exists on server "+CheckDupe->GetParent()->GetName()+"!");
					Srv->SendOpers("*** Server connection from \2"+servername+"\2 denied, already exists on server "+CheckDupe->GetParent()->GetName());
					return false;
				}
				// Begin the sync here. this kickstarts the
				// other side, waiting in WAIT_AUTH_2 state,
				// into starting their burst, as it shows
				// that we're happy.
				this->LinkState = CONNECTED;
				// we should add the details of this server now
				// to the servers tree, as a child of the root
				// node.
				TreeServer* Node = new TreeServer(servername,description,TreeRoot,this);
				TreeRoot->AddChild(Node);
				params[3] = ":" + params[3];
				DoOneToAllButSender(TreeRoot->GetName(),"SERVER",params,servername);
				this->bursting = true;
				this->DoBurst(Node);
				return true;
			}
		}
		this->WriteLine("ERROR :Invalid credentials");
		Srv->SendOpers("*** Server connection from \2"+servername+"\2 denied, invalid link credentials");
		return false;
	}

	bool Inbound_Server(std::deque<std::string> &params)
	{
		if (params.size() < 4)
			return false;
		std::string servername = params[0];
		std::string password = params[1];
		int hops = atoi(params[2].c_str());
		if (hops)
		{
			this->WriteLine("ERROR :Server too far away for authentication");
			Srv->SendOpers("*** Server connection from \2"+servername+"\2 denied, server is too far away for authentication");
			return false;
		}
		std::string description = params[3];
		for (std::vector<Link>::iterator x = LinkBlocks.begin(); x < LinkBlocks.end(); x++)
		{
			if ((x->Name == servername) && (x->RecvPass == password))
			{
				TreeServer* CheckDupe = FindServer(servername);
				if (CheckDupe)
				{
					this->WriteLine("ERROR :Server "+servername+" already exists on server "+CheckDupe->GetParent()->GetName()+"!");
					Srv->SendOpers("*** Server connection from \2"+servername+"\2 denied, already exists on server "+CheckDupe->GetParent()->GetName());
					return false;
				}
				/* If the config says this link is encrypted, but the remote side
				 * hasnt bothered to send the AES command before SERVER, then we
				 * boot them off as we MUST have this connection encrypted.
				 */
				if ((x->EncryptionKey != "") && (!this->ctx_in))
				{
					this->WriteLine("ERROR :This link requires AES encryption to be enabled. Plaintext connection refused.");
					Srv->SendOpers("*** Server connection from \2"+servername+"\2 denied, remote server did not enable AES.");
					return false;
				}
				Srv->SendOpers("*** Verified incoming server connection from \002"+servername+"\002["+this->GetIP()+"] ("+description+")");
				this->InboundServerName = servername;
				this->InboundDescription = description;
				// this is good. Send our details: Our server name and description and hopcount of 0,
				// along with the sendpass from this block.
				this->WriteLine("SERVER "+Srv->GetServerName()+" "+x->SendPass+" 0 :"+Srv->GetServerDescription());
				// move to the next state, we are now waiting for THEM.
				this->LinkState = WAIT_AUTH_2;
				return true;
			}
		}
		this->WriteLine("ERROR :Invalid credentials");
		Srv->SendOpers("*** Server connection from \2"+servername+"\2 denied, invalid link credentials");
		return false;
	}

	void Split(std::string line, bool stripcolon, std::deque<std::string> &n)
	{
		if (!strchr(line.c_str(),' '))
		{
			n.push_back(line);
			return;
		}
		std::stringstream s(line);
		std::string param = "";
		n.clear();
		int item = 0;
		while (!s.eof())
		{
			char c;
			s.get(c);
			if (c == ' ')
			{
				n.push_back(param);
				param = "";
				item++;
			}
			else
			{
				if (!s.eof())
				{
					param = param + c;
				}
				if ((param == ":") && (item > 0))
				{
					param = "";
					while (!s.eof())
					{
						s.get(c);
						if (!s.eof())
						{
							param = param + c;
						}
					}
					n.push_back(param);
					param = "";
				}
			}
		}
		if (param != "")
		{
			n.push_back(param);
		}
		return;
	}

	bool ProcessLine(std::string line)
	{
		char* l = (char*)line.c_str();
		while ((strlen(l)) && (l[strlen(l)-1] == '\r') || (l[strlen(l)-1] == '\n'))
			l[strlen(l)-1] = '\0';
		line = l;
		if (line == "")
			return true;
		Srv->Log(DEBUG,"IN: "+line);
		
		std::deque<std::string> params;
	        this->Split(line,true,params);
		std::string command = "";
		std::string prefix = "";
		if (((params[0].c_str())[0] == ':') && (params.size() > 1))
		{
			prefix = params[0];
			command = params[1];
			char* pref = (char*)prefix.c_str();
			prefix = ++pref;
			params.pop_front();
			params.pop_front();
		}
		else
		{
			prefix = "";
			command = params[0];
			params.pop_front();
		}

		if ((!this->ctx_in) && (command == "AES"))
		{
                        std::string sserv = params[0];
                        for (std::vector<Link>::iterator x = LinkBlocks.begin(); x < LinkBlocks.end(); x++)
                        {
                                if ((x->EncryptionKey != "") && (x->Name == sserv))
                                {
                                        this->InitAES(x->EncryptionKey,sserv);
                                }
                        }
                        return true;
		}
		else if ((this->ctx_in) && (command == "AES"))
		{
			WriteOpers("*** \2AES\2: Encryption already enabled on this connection yet %s is trying to enable it twice!",params[0].c_str());
		}

		switch (this->LinkState)
		{
			TreeServer* Node;
			
			case WAIT_AUTH_1:
				// Waiting for SERVER command from remote server. Server initiating
				// the connection sends the first SERVER command, listening server
				// replies with theirs if its happy, then if the initiator is happy,
				// it starts to send its net sync, which starts the merge, otherwise
				// it sends an ERROR.
				if (command == "PASS")
				{
					/* Silently ignored */
				}
				else if (command == "SERVER")
				{
					return this->Inbound_Server(params);
				}
				else if (command == "ERROR")
				{
					return this->Error(params);
				}
				else if (command == "USER")
				{
					this->WriteLine("ERROR :Client connections to this port are prohibited.");
					return false;
				}
				else if (command == "CAPAB")
				{
					return this->Capab(params);
				}
				else if ((command == "U") || (command == "S"))
				{
					this->WriteLine("ERROR :Cannot use the old-style mesh linking protocol with m_spanningtree.so!");
					return false;
				}
				else
				{
					this->WriteLine("ERROR :Invalid command in negotiation phase.");
					return false;
				}
			break;
			case WAIT_AUTH_2:
				// Waiting for start of other side's netmerge to say they liked our
				// password.
				if (command == "SERVER")
				{
					// cant do this, they sent it to us in the WAIT_AUTH_1 state!
					// silently ignore.
					return true;
				}
				else if ((command == "U") || (command == "S"))
				{
					this->WriteLine("ERROR :Cannot use the old-style mesh linking protocol with m_spanningtree.so!");
					return false;
				}
				else if (command == "BURST")
				{
					this->LinkState = CONNECTED;
					Node = new TreeServer(InboundServerName,InboundDescription,TreeRoot,this);
	                                TreeRoot->AddChild(Node);
					params.clear();
					params.push_back(InboundServerName);
					params.push_back("*");
					params.push_back("1");
					params.push_back(":"+InboundDescription);
					DoOneToAllButSender(TreeRoot->GetName(),"SERVER",params,InboundServerName);
					this->bursting = true;
	                                this->DoBurst(Node);
				}
				else if (command == "ERROR")
				{
					return this->Error(params);
				}
				else if (command == "CAPAB")
				{
					return this->Capab(params);
				}
				
			break;
			case LISTENER:
				this->WriteLine("ERROR :Internal error -- listening socket accepted its own descriptor!!!");
				return false;
			break;
			case CONNECTING:
				if (command == "SERVER")
				{
					// another server we connected to, which was in WAIT_AUTH_1 state,
					// has just sent us their credentials. If we get this far, theyre
					// happy with OUR credentials, and they are now in WAIT_AUTH_2 state.
					// if we're happy with this, we should send our netburst which
					// kickstarts the merge.
					return this->Outbound_Reply_Server(params);
				}
				else if (command == "ERROR")
				{
					return this->Error(params);
				}
			break;
			case CONNECTED:
				// This is the 'authenticated' state, when all passwords
				// have been exchanged and anything past this point is taken
				// as gospel.
				
				if (prefix != "")
				{
					std::string direction = prefix;
					userrec* t = Srv->FindNick(prefix);
					if (t)
					{
						direction = t->server;
					}
					TreeServer* route_back_again = BestRouteTo(direction);
					if ((!route_back_again) || (route_back_again->GetSocket() != this))
					{
						if (route_back_again)
							log(DEBUG,"Protocol violation: Fake direction in command '%s' from connection '%s'",line.c_str(),this->GetName().c_str());
						return true;
					}

			                /* Fix by brain:
					 * When there is activity on the socket, reset the ping counter so
					 * that we're not wasting bandwidth pinging an active server.
					 */                     
			                route_back_again->SetNextPingTime(time(NULL) + 300);
			                route_back_again->SetPingFlag();
				}
				
				if (command == "SVSMODE")
				{
					/* Services expects us to implement
					 * SVSMODE. In inspircd its the same as
					 * MODE anyway.
					 */
					command = "MODE";
				}
				std::string target = "";
				/* Yes, know, this is a mess. Its reasonably fast though as we're
				 * working with std::string here.
				 */
				if ((command == "NICK") && (params.size() > 1))
				{
					return this->IntroduceClient(prefix,params);
				}
				else if (command == "FJOIN")
				{
					return this->ForceJoin(prefix,params);
				}
				else if (command == "SERVER")
				{
					return this->RemoteServer(prefix,params);
				}
				else if (command == "ERROR")
				{
					return this->Error(params);
				}
				else if (command == "OPERTYPE")
				{
					return this->OperType(prefix,params);
				}
				else if (command == "FMODE")
				{
					return this->ForceMode(prefix,params);
				}
				else if (command == "KILL")
				{
					return this->RemoteKill(prefix,params);
				}
				else if (command == "FTOPIC")
				{
					return this->ForceTopic(prefix,params);
				}
				else if (command == "REHASH")
				{
					return this->RemoteRehash(prefix,params);
				}
				else if (command == "METADATA")
				{
					return this->MetaData(prefix,params);
				}
				else if (command == "PING")
				{
					return this->LocalPing(prefix,params);
				}
				else if (command == "PONG")
				{
					return this->LocalPong(prefix,params);
				}
				else if (command == "VERSION")
				{
					return this->ServerVersion(prefix,params);
				}
				else if (command == "FHOST")
				{
					return this->ChangeHost(prefix,params);
				}
				else if (command == "FNAME")
				{
					return this->ChangeName(prefix,params);
				}
				else if (command == "ADDLINE")
				{
					return this->AddLine(prefix,params);
				}
				else if (command == "SVSNICK")
				{
					if (prefix == "")
					{
						prefix = this->GetName();
					}
					return this->ForceNick(prefix,params);
				}
				else if (command == "IDLE")
				{
					return this->Whois(prefix,params);
				}
				else if (command == "SVSJOIN")
				{
					if (prefix == "")
					{
						prefix = this->GetName();
					}
					return this->ServiceJoin(prefix,params);
				}
				else if (command == "SQUIT")
				{
					if (params.size() == 2)
					{
						this->Squit(FindServer(params[0]),params[1]);
					}
					return true;
				}
				else if (command == "ENDBURST")
				{
					this->bursting = false;
					return true;
				}
				else
				{
					// not a special inter-server command.
					// Emulate the actual user doing the command,
					// this saves us having a huge ugly parser.
					userrec* who = Srv->FindNick(prefix);
					std::string sourceserv = this->myhost;
					if (this->InboundServerName != "")
					{
						sourceserv = this->InboundServerName;
					}
					if (who)
					{
						// its a user
						target = who->server;
						char* strparams[127];
						for (unsigned int q = 0; q < params.size(); q++)
						{
							strparams[q] = (char*)params[q].c_str();
						}
						Srv->CallCommandHandler(command, strparams, params.size(), who);
					}
					else
					{
						// its not a user. Its either a server, or somethings screwed up.
						if (IsServer(prefix))
						{
							target = Srv->GetServerName();
						}
						else
						{
							log(DEBUG,"Command with unknown origin '%s'",prefix.c_str());
							return true;
						}
					}
					return DoOneToAllButSenderRaw(line,sourceserv,prefix,command,params);

				}
				return true;
			break;
		}
		return true;
	}

	virtual std::string GetName()
	{
		std::string sourceserv = this->myhost;
		if (this->InboundServerName != "")
		{
			sourceserv = this->InboundServerName;
		}
		return sourceserv;
	}

	virtual void OnTimeout()
	{
		if (this->LinkState == CONNECTING)
		{
			Srv->SendOpers("*** CONNECT: Connection to \002"+myhost+"\002 timed out.");
		}
	}

	virtual void OnClose()
	{
		// Connection closed.
		// If the connection is fully up (state CONNECTED)
		// then propogate a netsplit to all peers.
		std::string quitserver = this->myhost;
		if (this->InboundServerName != "")
		{
			quitserver = this->InboundServerName;
		}
		TreeServer* s = FindServer(quitserver);
		if (s)
		{
			Squit(s,"Remote host closed the connection");
		}
		WriteOpers("Server '\2%s\2[%s]' closed the connection.",quitserver.c_str(),this->GetIP().c_str());
	}

	virtual int OnIncomingConnection(int newsock, char* ip)
	{
		TreeSocket* s = new TreeSocket(newsock, ip);
		Srv->AddSocket(s);
		return true;
	}
};

void AddThisServer(TreeServer* server, std::deque<TreeServer*> &list)
{
	for (unsigned int c = 0; c < list.size(); c++)
	{
		if (list[c] == server)
		{
			return;
		}
	}
	list.push_back(server);
}

// returns a list of DIRECT servernames for a specific channel
void GetListOfServersForChannel(chanrec* c, std::deque<TreeServer*> &list)
{
	std::vector<char*> *ulist = c->GetUsers();
	unsigned int ucount = ulist->size();
	for (unsigned int i = 0; i < ucount; i++)
	{
		char* o = (*ulist)[i];
		userrec* otheruser = (userrec*)o;
		if (otheruser->fd < 0)
		{
			TreeServer* best = BestRouteTo(otheruser->server);
			if (best)
				AddThisServer(best,list);
		}
	}
	return;
}

bool DoOneToAllButSenderRaw(std::string data, std::string omit, std::string prefix, std::string command, std::deque<std::string> &params)
{
	TreeServer* omitroute = BestRouteTo(omit);
	if ((command == "NOTICE") || (command == "PRIVMSG"))
	{
		if ((params.size() >= 2) && (*(params[0].c_str()) != '$'))
		{
			if (*(params[0].c_str()) != '#')
			{
				// special routing for private messages/notices
				userrec* d = Srv->FindNick(params[0]);
				if (d)
				{
					std::deque<std::string> par;
					par.push_back(params[0]);
					par.push_back(":"+params[1]);
					DoOneToOne(prefix,command,par,d->server);
					return true;
				}
			}
			else
			{
				log(DEBUG,"Channel privmsg going to chan %s",params[0].c_str());
				chanrec* c = Srv->FindChannel(params[0]);
				if (c)
				{
					std::deque<TreeServer*> list;
					GetListOfServersForChannel(c,list);
					log(DEBUG,"Got a list of %d servers",list.size());
					unsigned int lsize = list.size();
					for (unsigned int i = 0; i < lsize; i++)
					{
						TreeSocket* Sock = list[i]->GetSocket();
						if ((Sock) && (list[i]->GetName() != omit) && (omitroute != list[i]))
						{
							log(DEBUG,"Writing privmsg to server %s",list[i]->GetName().c_str());
							Sock->WriteLine(data);
						}
					}
					return true;
				}
			}
		}
	}
	unsigned int items = TreeRoot->ChildCount();
	for (unsigned int x = 0; x < items; x++)
	{
		TreeServer* Route = TreeRoot->GetChild(x);
		if ((Route->GetSocket()) && (Route->GetName() != omit) && (omitroute != Route))
		{
			TreeSocket* Sock = Route->GetSocket();
			Sock->WriteLine(data);
		}
	}
	return true;
}

bool DoOneToAllButSender(std::string prefix, std::string command, std::deque<std::string> &params, std::string omit)
{
	TreeServer* omitroute = BestRouteTo(omit);
	std::string FullLine = ":" + prefix + " " + command;
	unsigned int words = params.size();
	for (unsigned int x = 0; x < words; x++)
	{
		FullLine = FullLine + " " + params[x];
	}
	unsigned int items = TreeRoot->ChildCount();
	for (unsigned int x = 0; x < items; x++)
	{
		TreeServer* Route = TreeRoot->GetChild(x);
		// Send the line IF:
		// The route has a socket (its a direct connection)
		// The route isnt the one to be omitted
		// The route isnt the path to the one to be omitted
		if ((Route->GetSocket()) && (Route->GetName() != omit) && (omitroute != Route))
		{
			TreeSocket* Sock = Route->GetSocket();
			Sock->WriteLine(FullLine);
		}
	}
	return true;
}

bool DoOneToMany(std::string prefix, std::string command, std::deque<std::string> &params)
{
	std::string FullLine = ":" + prefix + " " + command;
	unsigned int words = params.size();
	for (unsigned int x = 0; x < words; x++)
	{
		FullLine = FullLine + " " + params[x];
	}
	unsigned int items = TreeRoot->ChildCount();
	for (unsigned int x = 0; x < items; x++)
	{
		TreeServer* Route = TreeRoot->GetChild(x);
		if (Route->GetSocket())
		{
			TreeSocket* Sock = Route->GetSocket();
			Sock->WriteLine(FullLine);
		}
	}
	return true;
}

bool DoOneToOne(std::string prefix, std::string command, std::deque<std::string> &params, std::string target)
{
	TreeServer* Route = BestRouteTo(target);
	if (Route)
	{
		std::string FullLine = ":" + prefix + " " + command;
		unsigned int words = params.size();
		for (unsigned int x = 0; x < words; x++)
		{
			FullLine = FullLine + " " + params[x];
		}
		if (Route->GetSocket())
		{
			TreeSocket* Sock = Route->GetSocket();
			Sock->WriteLine(FullLine);
		}
		return true;
	}
	else
	{
		return true;
	}
}

std::vector<TreeSocket*> Bindings;

void ReadConfiguration(bool rebind)
{
	Conf = new ConfigReader;
	if (rebind)
	{
		for (int j =0; j < Conf->Enumerate("bind"); j++)
		{
			std::string Type = Conf->ReadValue("bind","type",j);
			std::string IP = Conf->ReadValue("bind","address",j);
			long Port = Conf->ReadInteger("bind","port",j,true);
			if (Type == "servers")
			{
				if (IP == "*")
				{
					IP = "";
				}
				TreeSocket* listener = new TreeSocket(IP.c_str(),Port,true,10);
				if (listener->GetState() == I_LISTENING)
				{
					Srv->AddSocket(listener);
					Bindings.push_back(listener);
				}
				else
				{
					log(DEFAULT,"m_spanningtree: Warning: Failed to bind server port %d",Port);
					listener->Close();
					delete listener;
				}
			}
		}
	}
	LinkBlocks.clear();
	for (int j =0; j < Conf->Enumerate("link"); j++)
	{
		Link L;
		L.Name = Conf->ReadValue("link","name",j);
		L.IPAddr = Conf->ReadValue("link","ipaddr",j);
		L.Port = Conf->ReadInteger("link","port",j,true);
		L.SendPass = Conf->ReadValue("link","sendpass",j);
		L.RecvPass = Conf->ReadValue("link","recvpass",j);
		L.AutoConnect = Conf->ReadInteger("link","autoconnect",j,true);
		L.EncryptionKey =  Conf->ReadValue("link","encryptionkey",j);
		L.NextConnectTime = time(NULL) + L.AutoConnect;
		/* Bugfix by brain, do not allow people to enter bad configurations */
		if ((L.RecvPass != "") && (L.SendPass != "") && (L.Name != "") && (L.Port))
		{
			LinkBlocks.push_back(L);
			log(DEBUG,"m_spanningtree: Read server %s with host %s:%d",L.Name.c_str(),L.IPAddr.c_str(),L.Port);
		}
		else
		{
			if (L.RecvPass == "")
			{
				log(DEFAULT,"Invalid configuration for server '%s', recvpass not defined!",L.Name.c_str());
			}
			else if (L.SendPass == "")
			{
				log(DEFAULT,"Invalid configuration for server '%s', sendpass not defined!",L.Name.c_str());
			}
			else if (L.Name == "")
			{
				log(DEFAULT,"Invalid configuration, link tag without a name!");
			}
			else if (!L.Port)
			{
				log(DEFAULT,"Invalid configuration for server '%s', no port specified!",L.Name.c_str());
			}
		}
	}
	delete Conf;
}


class ModuleSpanningTree : public Module
{
	std::vector<TreeSocket*> Bindings;
	int line;
	int NumServers;

 public:

	ModuleSpanningTree(Server* Me)
		: Module::Module(Me)
	{
		Srv = Me;
		Bindings.clear();

		// Create the root of the tree
		TreeRoot = new TreeServer(Srv->GetServerName(),Srv->GetServerDescription());

		ReadConfiguration(true);
	}

	void ShowLinks(TreeServer* Current, userrec* user, int hops)
	{
		std::string Parent = TreeRoot->GetName();
		if (Current->GetParent())
		{
			Parent = Current->GetParent()->GetName();
		}
		for (unsigned int q = 0; q < Current->ChildCount(); q++)
		{
			ShowLinks(Current->GetChild(q),user,hops+1);
		}
		WriteServ(user->fd,"364 %s %s %s :%d %s",user->nick,Current->GetName().c_str(),Parent.c_str(),hops,Current->GetDesc().c_str());
	}

	int CountLocalServs()
	{
		return TreeRoot->ChildCount();
	}

	int CountServs()
	{
		return serverlist.size();
	}

	void HandleLinks(char** parameters, int pcnt, userrec* user)
	{
		ShowLinks(TreeRoot,user,0);
		WriteServ(user->fd,"365 %s * :End of /LINKS list.",user->nick);
		return;
	}

	void HandleLusers(char** parameters, int pcnt, userrec* user)
	{
		WriteServ(user->fd,"251 %s :There are %d users and %d invisible on %d servers",user->nick,usercnt()-usercount_invisible(),usercount_invisible(),this->CountServs());
		WriteServ(user->fd,"252 %s %d :operator(s) online",user->nick,usercount_opers());
		WriteServ(user->fd,"253 %s %d :unknown connections",user->nick,usercount_unknown());
		WriteServ(user->fd,"254 %s %d :channels formed",user->nick,chancount());
		WriteServ(user->fd,"254 %s :I have %d clients and %d servers",user->nick,local_count(),this->CountLocalServs());
		return;
	}

	// WARNING: NOT THREAD SAFE - DONT GET ANY SMART IDEAS.

	void ShowMap(TreeServer* Current, userrec* user, int depth, char matrix[128][80])
	{
		if (line < 128)
		{
			for (int t = 0; t < depth; t++)
			{
				matrix[line][t] = ' ';
			}

			// For Aligning, we need to work out exactly how deep this thing is, and produce
			// a 'Spacer' String to compensate.
			char spacer[40];

			memset(spacer,' ',40);
			if ((40 - Current->GetName().length() - depth) > 1) {
				spacer[40 - Current->GetName().length() - depth] = '\0';
			}
			else
			{
				spacer[5] = '\0';
			}

			float percent;
			char text[80];
			if (clientlist.size() == 0) {
				// If there are no users, WHO THE HELL DID THE /MAP?!?!?!
				percent = 0;
			}
			else
			{
				percent = ((float)Current->GetUserCount() / (float)clientlist.size()) * 100;
			}
			snprintf(text, 80, "%s %s%5d [%5.2f%%]", Current->GetName().c_str(), spacer, Current->GetUserCount(), percent);
			strlcpy(&matrix[line][depth],text,80);
			line++;
			for (unsigned int q = 0; q < Current->ChildCount(); q++)
			{
				ShowMap(Current->GetChild(q),user,depth+2,matrix);
			}
		}
	}

	// Ok, prepare to be confused.
	// After much mulling over how to approach this, it struck me that
	// the 'usual' way of doing a /MAP isnt the best way. Instead of
	// keeping track of a ton of ascii characters, and line by line
	// under recursion working out where to place them using multiplications
	// and divisons, we instead render the map onto a backplane of characters
	// (a character matrix), then draw the branches as a series of "L" shapes
	// from the nodes. This is not only friendlier on CPU it uses less stack.

	void HandleMap(char** parameters, int pcnt, userrec* user)
	{
		// This array represents a virtual screen which we will
		// "scratch" draw to, as the console device of an irc
		// client does not provide for a proper terminal.
		char matrix[128][80];
		for (unsigned int t = 0; t < 128; t++)
		{
			matrix[t][0] = '\0';
		}
		line = 0;
		// The only recursive bit is called here.
		ShowMap(TreeRoot,user,0,matrix);
		// Process each line one by one. The algorithm has a limit of
		// 128 servers (which is far more than a spanning tree should have
		// anyway, so we're ok). This limit can be raised simply by making
		// the character matrix deeper, 128 rows taking 10k of memory.
		for (int l = 1; l < line; l++)
		{
			// scan across the line looking for the start of the
			// servername (the recursive part of the algorithm has placed
			// the servers at indented positions depending on what they
			// are related to)
			int first_nonspace = 0;
			while (matrix[l][first_nonspace] == ' ')
			{
				first_nonspace++;
			}
			first_nonspace--;
			// Draw the `- (corner) section: this may be overwritten by
			// another L shape passing along the same vertical pane, becoming
			// a |- (branch) section instead.
			matrix[l][first_nonspace] = '-';
			matrix[l][first_nonspace-1] = '`';
			int l2 = l - 1;
			// Draw upwards until we hit the parent server, causing possibly
			// other corners (`-) to become branches (|-)
			while ((matrix[l2][first_nonspace-1] == ' ') || (matrix[l2][first_nonspace-1] == '`'))
			{
				matrix[l2][first_nonspace-1] = '|';
				l2--;
			}
		}
		// dump the whole lot to the user. This is the easy bit, honest.
		for (int t = 0; t < line; t++)
		{
			WriteServ(user->fd,"006 %s :%s",user->nick,&matrix[t][0]);
		}
	        WriteServ(user->fd,"007 %s :End of /MAP",user->nick);
		return;
	}

	int HandleSquit(char** parameters, int pcnt, userrec* user)
	{
		TreeServer* s = FindServerMask(parameters[0]);
		if (s)
		{
			if (s == TreeRoot)
			{
				 WriteServ(user->fd,"NOTICE %s :*** SQUIT: Foolish mortal, you cannot make a server SQUIT itself! (%s matches local server name)",user->nick,parameters[0]);
				return 1;
			}
			TreeSocket* sock = s->GetSocket();
			if (sock)
			{
				log(DEBUG,"Splitting server %s",s->GetName().c_str());
				WriteOpers("*** SQUIT: Server \002%s\002 removed from network by %s",parameters[0],user->nick);
				sock->Squit(s,"Server quit by "+std::string(user->nick)+"!"+std::string(user->ident)+"@"+std::string(user->host));
				sock->Close();
			}
			else
			{
				WriteServ(user->fd,"NOTICE %s :*** SQUIT: The server \002%s\002 is not directly connected.",user->nick,parameters[0]);
			}
		}
		else
		{
			 WriteServ(user->fd,"NOTICE %s :*** SQUIT: The server \002%s\002 does not exist on the network.",user->nick,parameters[0]);
		}
		return 1;
	}

	int HandleRemoteWhois(char** parameters, int pcnt, userrec* user)
	{
		if ((user->fd > -1) && (pcnt > 1))
		{
			userrec* remote = Srv->FindNick(parameters[1]);
			if ((remote) && (remote->fd < 0))
			{
				std::deque<std::string> params;
				params.push_back(parameters[1]);
				DoOneToOne(user->nick,"IDLE",params,remote->server);
				return 1;
			}
			else if (!remote)
			{
		                WriteServ(user->fd,"401 %s %s :No such nick/channel",user->nick, parameters[1]);
		                WriteServ(user->fd,"318 %s %s :End of /WHOIS list.",user->nick, parameters[1]);
				return 1;
			}
		}
		return 0;
	}

	void DoPingChecks(time_t curtime)
	{
		for (unsigned int j = 0; j < TreeRoot->ChildCount(); j++)
		{
			TreeServer* serv = TreeRoot->GetChild(j);
			TreeSocket* sock = serv->GetSocket();
			if (sock)
			{
				if (curtime >= serv->NextPingTime())
				{
					if (serv->AnsweredLastPing())
					{
						sock->WriteLine(":"+Srv->GetServerName()+" PING "+serv->GetName());
						serv->SetNextPingTime(curtime + 300);
					}
					else
					{
						// they didnt answer, boot them
						WriteOpers("*** Server \002%s\002 pinged out",serv->GetName().c_str());
						sock->Squit(serv,"Ping timeout");
						sock->Close();
						return;
					}
				}
			}
		}
	}

	void AutoConnectServers(time_t curtime)
	{
		for (std::vector<Link>::iterator x = LinkBlocks.begin(); x < LinkBlocks.end(); x++)
		{
			if ((x->AutoConnect) && (curtime >= x->NextConnectTime))
			{
				log(DEBUG,"Auto-Connecting %s",x->Name.c_str());
				x->NextConnectTime = curtime + x->AutoConnect;
				TreeServer* CheckDupe = FindServer(x->Name);
				if (!CheckDupe)
				{
					// an autoconnected server is not connected. Check if its time to connect it
					WriteOpers("*** AUTOCONNECT: Auto-connecting server \002%s\002 (%lu seconds until next attempt)",x->Name.c_str(),x->AutoConnect);
					TreeSocket* newsocket = new TreeSocket(x->IPAddr,x->Port,false,10,x->Name);
					Srv->AddSocket(newsocket);
				}
			}
		}
	}

	int HandleVersion(char** parameters, int pcnt, userrec* user)
	{
		// we've already checked if pcnt > 0, so this is safe
		TreeServer* found = FindServerMask(parameters[0]);
		if (found)
		{
			std::string Version = found->GetVersion();
			WriteServ(user->fd,"351 %s :%s",user->nick,Version.c_str());
			if (found == TreeRoot)
			{
			        std::stringstream out(Config->data005);
			        std::string token = "";
			        std::string line5 = "";
			        int token_counter = 0;
			        while (!out.eof())
			        {
			                out >> token;
			                line5 = line5 + token + " ";   
			                token_counter++;
			                if ((token_counter >= 13) || (out.eof() == true))
			                {
			                        WriteServ(user->fd,"005 %s %s:are supported by this server",user->nick,line5.c_str());
			                        line5 = "";
			                        token_counter = 0;
			                }
			        }
			}
		}
		else
		{
			WriteServ(user->fd,"402 %s %s :No such server",user->nick,parameters[0]);
		}
		return 1;
	}
	
	int HandleConnect(char** parameters, int pcnt, userrec* user)
	{
		for (std::vector<Link>::iterator x = LinkBlocks.begin(); x < LinkBlocks.end(); x++)
		{
			if (Srv->MatchText(x->Name.c_str(),parameters[0]))
			{
				TreeServer* CheckDupe = FindServer(x->Name);
				if (!CheckDupe)
				{
					WriteServ(user->fd,"NOTICE %s :*** CONNECT: Connecting to server: \002%s\002 (%s:%d)",user->nick,x->Name.c_str(),x->IPAddr.c_str(),x->Port);
					TreeSocket* newsocket = new TreeSocket(x->IPAddr,x->Port,false,10,x->Name);
					Srv->AddSocket(newsocket);
					return 1;
				}
				else
				{
					WriteServ(user->fd,"NOTICE %s :*** CONNECT: Server \002%s\002 already exists on the network and is connected via \002%s\002",user->nick,x->Name.c_str(),CheckDupe->GetParent()->GetName().c_str());
					return 1;
				}
			}
		}
		WriteServ(user->fd,"NOTICE %s :*** CONNECT: No server matching \002%s\002 could be found in the config file.",user->nick,parameters[0]);
		return 1;
	}

	virtual int OnStats(char statschar, userrec* user)
	{
		if (statschar == 'c')
	        {
			for (unsigned int i = 0; i < LinkBlocks.size(); i++)
			{
				WriteServ(user->fd,"213 %s C *@%s * %s %d 0 %c%c%c",user->nick,LinkBlocks[i].IPAddr.c_str(),LinkBlocks[i].Name.c_str(),LinkBlocks[i].Port,(LinkBlocks[i].EncryptionKey != "" ? 'e' : '-'),(LinkBlocks[i].AutoConnect ? 'a' : '-'),'s');
				WriteServ(user->fd,"244 %s H * * %s",user->nick,LinkBlocks[i].Name.c_str());
			}
			WriteServ(user->fd,"219 %s %c :End of /STATS report",user->nick,statschar);
			WriteOpers("*** Notice: Stats '%c' requested by %s (%s@%s)",statschar,user->nick,user->ident,user->host);
			return 1;
		}
		return 0;
	}

	virtual int OnPreCommand(std::string command, char **parameters, int pcnt, userrec *user, bool validated)
	{
		/* If the command doesnt appear to be valid, we dont want to mess with it. */
		if (!validated)
			return 0;

		if (command == "CONNECT")
		{
			return this->HandleConnect(parameters,pcnt,user);
		}
		else if (command == "SQUIT")
		{
			return this->HandleSquit(parameters,pcnt,user);
		}
		else if (command == "MAP")
		{
			this->HandleMap(parameters,pcnt,user);
			return 1;
		}
		else if (command == "LUSERS")
		{
			this->HandleLusers(parameters,pcnt,user);
			return 1;
		}
		else if (command == "LINKS")
		{
			this->HandleLinks(parameters,pcnt,user);
			return 1;
		}
		else if (command == "WHOIS")
		{
			if (pcnt > 1)
			{
				// remote whois
				return this->HandleRemoteWhois(parameters,pcnt,user);
			}
		}
		else if ((command == "VERSION") && (pcnt > 0))
		{
			this->HandleVersion(parameters,pcnt,user);
			return 1;
		}
		else if (Srv->IsValidModuleCommand(command, pcnt, user))
		{
			// this bit of code cleverly routes all module commands
			// to all remote severs *automatically* so that modules
			// can just handle commands locally, without having
			// to have any special provision in place for remote
			// commands and linking protocols.
			std::deque<std::string> params;
			params.clear();
			for (int j = 0; j < pcnt; j++)
			{
				if (strchr(parameters[j],' '))
				{
					params.push_back(":" + std::string(parameters[j]));
				}
				else
				{
					params.push_back(std::string(parameters[j]));
				}
			}
			DoOneToMany(user->nick,command,params);
		}
		return 0;
	}

	virtual void OnGetServerDescription(std::string servername,std::string &description)
	{
		TreeServer* s = FindServer(servername);
		if (s)
		{
			description = s->GetDesc();
		}
	}

	virtual void OnUserInvite(userrec* source,userrec* dest,chanrec* channel)
	{
		if (source->fd > -1)
		{
			std::deque<std::string> params;
			params.push_back(dest->nick);
			params.push_back(channel->name);
			DoOneToMany(source->nick,"INVITE",params);
		}
	}

	virtual void OnPostLocalTopicChange(userrec* user, chanrec* chan, std::string topic)
	{
		std::deque<std::string> params;
		params.push_back(chan->name);
		params.push_back(":"+topic);
		DoOneToMany(user->nick,"TOPIC",params);
	}

	virtual void OnWallops(userrec* user, std::string text)
	{
		if (user->fd > -1)
		{
			std::deque<std::string> params;
			params.push_back(":"+text);
			DoOneToMany(user->nick,"WALLOPS",params);
		}
	}

	virtual void OnUserNotice(userrec* user, void* dest, int target_type, std::string text)
	{
		if (target_type == TYPE_USER)
		{
			userrec* d = (userrec*)dest;
			if ((d->fd < 0) && (user->fd > -1))
			{
				std::deque<std::string> params;
				params.clear();
				params.push_back(d->nick);
				params.push_back(":"+text);
				DoOneToOne(user->nick,"NOTICE",params,d->server);
			}
		}
		else
		{
			if (user->fd > -1)
			{
				chanrec *c = (chanrec*)dest;
				std::deque<TreeServer*> list;
				GetListOfServersForChannel(c,list);
				unsigned int ucount = list.size();
				for (unsigned int i = 0; i < ucount; i++)
				{
					TreeSocket* Sock = list[i]->GetSocket();
					if (Sock)
						Sock->WriteLine(":"+std::string(user->nick)+" NOTICE "+std::string(c->name)+" :"+text);
				}
			}
		}
	}

	virtual void OnUserMessage(userrec* user, void* dest, int target_type, std::string text)
	{
		if (target_type == TYPE_USER)
		{
			// route private messages which are targetted at clients only to the server
			// which needs to receive them
			userrec* d = (userrec*)dest;
			if ((d->fd < 0) && (user->fd > -1))
			{
				std::deque<std::string> params;
				params.clear();
				params.push_back(d->nick);
				params.push_back(":"+text);
				DoOneToOne(user->nick,"PRIVMSG",params,d->server);
			}
		}
		else
		{
			if (user->fd > -1)
			{
				chanrec *c = (chanrec*)dest;
				std::deque<TreeServer*> list;
				GetListOfServersForChannel(c,list);
				unsigned int ucount = list.size();
				for (unsigned int i = 0; i < ucount; i++)
				{
					TreeSocket* Sock = list[i]->GetSocket();
					if (Sock)
						Sock->WriteLine(":"+std::string(user->nick)+" PRIVMSG "+std::string(c->name)+" :"+text);
				}
			}
		}
	}

	virtual void OnBackgroundTimer(time_t curtime)
	{
		AutoConnectServers(curtime);
		DoPingChecks(curtime);
	}

	virtual void OnUserJoin(userrec* user, chanrec* channel)
	{
		// Only do this for local users
		if (user->fd > -1)
		{
			std::deque<std::string> params;
			params.clear();
			params.push_back(channel->name);
			if (*channel->key)
			{
				// if the channel has a key, force the join by emulating the key.
				params.push_back(channel->key);
			}
			if (channel->GetUserCounter() > 1)
			{
				// not the first in the channel
				DoOneToMany(user->nick,"JOIN",params);
			}
			else
			{
				// first in the channel, set up their permissions
				// and the channel TS with FJOIN.
				char ts[24];
				snprintf(ts,24,"%lu",(unsigned long)channel->age);
				params.clear();
				params.push_back(channel->name);
				params.push_back(ts);
				params.push_back("@"+std::string(user->nick));
				DoOneToMany(Srv->GetServerName(),"FJOIN",params);
			}
		}
	}

	virtual void OnChangeHost(userrec* user, std::string newhost)
	{
		// only occurs for local clients
		if (user->registered != 7)
			return;
		std::deque<std::string> params;
		params.push_back(newhost);
		DoOneToMany(user->nick,"FHOST",params);
	}

	virtual void OnChangeName(userrec* user, std::string gecos)
	{
		// only occurs for local clients
		if (user->registered != 7)
			return;
		std::deque<std::string> params;
		params.push_back(gecos);
		DoOneToMany(user->nick,"FNAME",params);
	}

	virtual void OnUserPart(userrec* user, chanrec* channel)
	{
		if (user->fd > -1)
		{
			std::deque<std::string> params;
			params.push_back(channel->name);
			DoOneToMany(user->nick,"PART",params);
		}
	}

	virtual void OnUserConnect(userrec* user)
	{
		char agestr[MAXBUF];
		if (user->fd > -1)
		{
			std::deque<std::string> params;
			snprintf(agestr,MAXBUF,"%lu",(unsigned long)user->age);
			params.push_back(agestr);
			params.push_back(user->nick);
			params.push_back(user->host);
			params.push_back(user->dhost);
			params.push_back(user->ident);
			params.push_back("+"+std::string(user->modes));
			params.push_back(user->ip);
			params.push_back(":"+std::string(user->fullname));
			DoOneToMany(Srv->GetServerName(),"NICK",params);

			// User is Local, change needs to be reflected!
			TreeServer* SourceServer = FindServer(user->server);
			if (SourceServer) {
				SourceServer->AddUserCount();
			}

		}
	}

	virtual void OnUserQuit(userrec* user, std::string reason)
	{
		if ((user->fd > -1) && (user->registered == 7))
		{
			std::deque<std::string> params;
			params.push_back(":"+reason);
			DoOneToMany(user->nick,"QUIT",params);
		}
		// Regardless, We need to modify the user Counts..
		TreeServer* SourceServer = FindServer(user->server);
		if (SourceServer) {
			SourceServer->DelUserCount();
		}

	}

	virtual void OnUserPostNick(userrec* user, std::string oldnick)
	{
		if (user->fd > -1)
		{
			std::deque<std::string> params;
			params.push_back(user->nick);
			DoOneToMany(oldnick,"NICK",params);
		}
	}

	virtual void OnUserKick(userrec* source, userrec* user, chanrec* chan, std::string reason)
	{
		if (source->fd > -1)
		{
			std::deque<std::string> params;
			params.push_back(chan->name);
			params.push_back(user->nick);
			params.push_back(":"+reason);
			DoOneToMany(source->nick,"KICK",params);
		}
	}

	virtual void OnRemoteKill(userrec* source, userrec* dest, std::string reason)
	{
		std::deque<std::string> params;
		params.push_back(dest->nick);
		params.push_back(":"+reason);
		DoOneToMany(source->nick,"KILL",params);
	}

	virtual void OnRehash(std::string parameter)
	{
		if (parameter != "")
		{
			std::deque<std::string> params;
			params.push_back(parameter);
			DoOneToMany(Srv->GetServerName(),"REHASH",params);
			// check for self
			if (Srv->MatchText(Srv->GetServerName(),parameter))
			{
				Srv->SendOpers("*** Remote rehash initiated from server \002"+Srv->GetServerName()+"\002.");
				Srv->RehashServer();
			}
		}
		ReadConfiguration(false);
	}

	// note: the protocol does not allow direct umode +o except
	// via NICK with 8 params. sending OPERTYPE infers +o modechange
	// locally.
	virtual void OnOper(userrec* user, std::string opertype)
	{
		if (user->fd > -1)
		{
			std::deque<std::string> params;
			params.push_back(opertype);
			DoOneToMany(user->nick,"OPERTYPE",params);
		}
	}

	void OnLine(userrec* source, std::string host, bool adding, char linetype, long duration, std::string reason)
	{
		if (source->fd > -1)
		{
			char type[8];
			snprintf(type,8,"%cLINE",linetype);
			std::string stype = type;
			if (adding)
			{
				char sduration[MAXBUF];
				snprintf(sduration,MAXBUF,"%ld",duration);
				std::deque<std::string> params;
				params.push_back(host);
				params.push_back(sduration);
				params.push_back(":"+reason);
				DoOneToMany(source->nick,stype,params);
			}
			else
			{
				std::deque<std::string> params;
				params.push_back(host);
				DoOneToMany(source->nick,stype,params);
			}
		}
	}

	virtual void OnAddGLine(long duration, userrec* source, std::string reason, std::string hostmask)
	{
		OnLine(source,hostmask,true,'G',duration,reason);
	}
	
	virtual void OnAddZLine(long duration, userrec* source, std::string reason, std::string ipmask)
	{
		OnLine(source,ipmask,true,'Z',duration,reason);
	}

	virtual void OnAddQLine(long duration, userrec* source, std::string reason, std::string nickmask)
	{
		OnLine(source,nickmask,true,'Q',duration,reason);
	}

	virtual void OnAddELine(long duration, userrec* source, std::string reason, std::string hostmask)
	{
		OnLine(source,hostmask,true,'E',duration,reason);
	}

	virtual void OnDelGLine(userrec* source, std::string hostmask)
	{
		OnLine(source,hostmask,false,'G',0,"");
	}

	virtual void OnDelZLine(userrec* source, std::string ipmask)
	{
		OnLine(source,ipmask,false,'Z',0,"");
	}

	virtual void OnDelQLine(userrec* source, std::string nickmask)
	{
		OnLine(source,nickmask,false,'Q',0,"");
	}

	virtual void OnDelELine(userrec* source, std::string hostmask)
	{
		OnLine(source,hostmask,false,'E',0,"");
	}

	virtual void OnMode(userrec* user, void* dest, int target_type, std::string text)
	{
		if ((user->fd > -1) && (user->registered == 7))
		{
			if (target_type == TYPE_USER)
			{
				userrec* u = (userrec*)dest;
				std::deque<std::string> params;
				params.push_back(u->nick);
				params.push_back(text);
				DoOneToMany(user->nick,"MODE",params);
			}
			else
			{
				chanrec* c = (chanrec*)dest;
				std::deque<std::string> params;
				params.push_back(c->name);
				params.push_back(text);
				DoOneToMany(user->nick,"MODE",params);
			}
		}
	}

	virtual void ProtoSendMode(void* opaque, int target_type, void* target, std::string modeline)
	{
		TreeSocket* s = (TreeSocket*)opaque;
		if (target)
		{
			if (target_type == TYPE_USER)
			{
				userrec* u = (userrec*)target;
				s->WriteLine(":"+Srv->GetServerName()+" FMODE "+u->nick+" "+modeline);
			}
			else
			{
				chanrec* c = (chanrec*)target;
				s->WriteLine(":"+Srv->GetServerName()+" FMODE "+c->name+" "+modeline);
			}
		}
	}

	virtual void ProtoSendMetaData(void* opaque, int target_type, void* target, std::string extname, std::string extdata)
	{
		TreeSocket* s = (TreeSocket*)opaque;
		if (target)
		{
			if (target_type == TYPE_USER)
			{
				userrec* u = (userrec*)target;
				s->WriteLine(":"+Srv->GetServerName()+" METADATA "+u->nick+" "+extname+" :"+extdata);
			}
			else
			{
				chanrec* c = (chanrec*)target;
				s->WriteLine(":"+Srv->GetServerName()+" METADATA "+c->name+" "+extname+" :"+extdata);
			}
		}
	}

	virtual ~ModuleSpanningTree()
	{
	}

	virtual Version GetVersion()
	{
		return Version(1,0,0,0,VF_STATIC|VF_VENDOR);
	}

	void Implements(char* List)
	{
		List[I_OnPreCommand] = List[I_OnGetServerDescription] = List[I_OnUserInvite] = List[I_OnPostLocalTopicChange] = 1;
		List[I_OnWallops] = List[I_OnUserNotice] = List[I_OnUserMessage] = List[I_OnBackgroundTimer] = 1;
		List[I_OnUserJoin] = List[I_OnChangeHost] = List[I_OnChangeName] = List[I_OnUserPart] = List[I_OnUserConnect] = 1;
		List[I_OnUserQuit] = List[I_OnUserPostNick] = List[I_OnUserKick] = List[I_OnRemoteKill] = List[I_OnRehash] = 1;
		List[I_OnOper] = List[I_OnAddGLine] = List[I_OnAddZLine] = List[I_OnAddQLine] = List[I_OnAddELine] = 1;
		List[I_OnDelGLine] = List[I_OnDelZLine] = List[I_OnDelQLine] = List[I_OnDelELine] = List[I_ProtoSendMode] = List[I_OnMode] = 1;
		List[I_OnStats] = List[I_ProtoSendMetaData] = 1;
	}

	/* It is IMPORTANT that m_spanningtree is the last module in the chain
	 * so that any activity it sees is FINAL, e.g. we arent going to send out
	 * a NICK message before m_cloaking has finished putting the +x on the user,
	 * etc etc.
	 * Therefore, we return PRIORITY_LAST to make sure we end up at the END of
	 * the module call queue.
	 */
	Priority Prioritize()
	{
		return PRIORITY_LAST;
	}
};


class ModuleSpanningTreeFactory : public ModuleFactory
{
 public:
	ModuleSpanningTreeFactory()
	{
	}
	
	~ModuleSpanningTreeFactory()
	{
	}
	
	virtual Module * CreateModule(Server* Me)
	{
		TreeProtocolModule = new ModuleSpanningTree(Me);
		return TreeProtocolModule;
	}
	
};


extern "C" void * init_module( void )
{
	return new ModuleSpanningTreeFactory;
}
