/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2008 InspIRCd Development Team
 * See: http://www.inspircd.org/wiki/index.php/Credits
 *
 * This program is free but copyrighted software; see
 *	    the file COPYING for details.
 *
 * ---------------------------------------------------
 */

/* $Core: libIRCDserver */

#include <signal.h>
#include "exitcodes.h"
#include "inspircd.h"


void InspIRCd::SignalHandler(int signal)
{
	switch (signal)
	{
		case SIGHUP:
			Rehash();
			break;
		case SIGTERM:
			Exit(signal);
			break;
	}
}

void InspIRCd::Exit(int status)
{
#ifdef WINDOWS
	delete WindowsIPC;
#endif
	if (this)
	{
		this->SendError("Exiting with status " + ConvToStr(status) + " (" + std::string(ExitCodes[status]) + ")");
		this->Cleanup();
	}
	exit (status);
}

void InspIRCd::Rehash()
{
	this->SNO->WriteToSnoMask('A', "Rehashing config file %s due to SIGHUP",ServerConfig::CleanFilename(this->ConfigFileName));
	this->RehashUsersAndChans();
	FOREACH_MOD_I(this, I_OnGarbageCollect, OnGarbageCollect());
	if (!this->ConfigThread)
	{
		Config->RehashUser = NULL;
		Config->RehashParameter = "";

		ConfigThread = new ConfigReaderThread(this, false, NULL);
		Threads->Create(ConfigThread);
	}
}

void InspIRCd::RehashServer()
{
	this->SNO->WriteToSnoMask('A', "Rehashing config file");
	this->RehashUsersAndChans();
	if (!this->ConfigThread)
	{
		Config->RehashUser = NULL;
		Config->RehashParameter = "";

		ConfigThread = new ConfigReaderThread(this, false, NULL);
		Threads->Create(ConfigThread);
	}
}

std::string InspIRCd::GetVersionString()
{
	char versiondata[MAXBUF];
	if (*Config->CustomVersion)
	{
		snprintf(versiondata,MAXBUF,"%s %s :%s",VERSION,Config->ServerName,Config->CustomVersion);
	}
	else
	{
		snprintf(versiondata,MAXBUF,"%s %s :%s [FLAGS=%s,%s,%s]",VERSION,Config->ServerName,SYSTEM,REVISION,SE->GetName().c_str(),Config->sid);
	}
	return versiondata;
}

void InspIRCd::BuildISupport()
{
	// the neatest way to construct the initial 005 numeric, considering the number of configure constants to go in it...
	std::stringstream v;
	v << "WALLCHOPS WALLVOICES MODES=" << MAXMODES << " CHANTYPES=# PREFIX=" << this->Modes->BuildPrefixes() << " MAP MAXCHANNELS=" << Config->MaxChans << " MAXBANS=60 VBANLIST NICKLEN=" << NICKMAX-1;
	v << " CASEMAPPING=rfc1459 STATUSMSG=@" << (this->Config->AllowHalfop ? "%" : "") << "+ CHARSET=ascii TOPICLEN=" << MAXTOPIC << " KICKLEN=" << MAXKICK << " MAXTARGETS=" << Config->MaxTargets;
	v << " AWAYLEN=" << MAXAWAY << " CHANMODES=" << this->Modes->ChanModes() << " FNC NETWORK=" << Config->Network << " MAXPARA=32 ELIST=MU";
	Config->data005 = v.str();
	FOREACH_MOD_I(this,I_On005Numeric,On005Numeric(Config->data005));
	Config->Update005();
}

std::string InspIRCd::GetRevision()
{
	return REVISION;
}

void InspIRCd::AddServerName(const std::string &servername)
{
	servernamelist::iterator itr = servernames.begin();
	for(; itr != servernames.end(); ++itr)
		if(**itr == servername)
			return;

	std::string * ns = new std::string(servername);
	servernames.push_back(ns);
}

const char* InspIRCd::FindServerNamePtr(const std::string &servername)
{
	servernamelist::iterator itr = servernames.begin();
	for(; itr != servernames.end(); ++itr)
		if(**itr == servername)
			return (*itr)->c_str();

	servernames.push_back(new std::string(servername));
	itr = --servernames.end();
	return (*itr)->c_str();
}

bool InspIRCd::FindServerName(const std::string &servername)
{
	servernamelist::iterator itr = servernames.begin();
	for(; itr != servernames.end(); ++itr)
		if(**itr == servername)
			return true;
	return false;
}

/*
 * Retrieve the next valid UUID that is free for this server.
 */
std::string InspIRCd::GetUID()
{
	static int curindex = -1;

	if (curindex == -1)
	{
		// Starting up
		current_uid[0] = Config->sid[0];
		current_uid[1] = Config->sid[1];
		current_uid[2] = Config->sid[2];

		for (int i = 3; i < UUID_LENGTH; i++)
			current_uid[i] = 'Z';

		current_uid[3] = 'Y'; // force fake client to get ZZZZZZZZ

		curindex = 3;
	}

	while (1)
	{
		printf("Getting ID. curindex %d, current_uid %s\n", curindex, current_uid);

		if (curindex == 3)
		{
			// Down to the last few.
			if (current_uid[curindex] == 'Z')
			{
				// Reset.
				for (int i = 3; i < UUID_LENGTH; i++)
				{
					current_uid[i] = 'A';
					curindex  = UUID_LENGTH - 1;
				}
			}
			else
				current_uid[curindex]++;
		}
		else
		{
			if (current_uid[curindex] == 'Z')
				current_uid[curindex] = '0';
			else if (current_uid[curindex] == '9')
			{
				current_uid[curindex] = 'A';
				curindex--;
				continue;
			}
			else
				current_uid[curindex]++;
		}

			if (this->FindUUID(current_uid))
			{
				/*
				 * It's in use. We need to try the loop again.
				 */
				continue;
			}

			return current_uid;
	}

#if 0

	/*
	 * This will only finish once we return a UUID that is not in use.
	 */
	while (1)
	{
		/*
		 * Okay. The rules for generating a UID go like this...
		 * -- > ABCDEFGHIJKLMNOPQRSTUVWXYZ --> 012345679 --> WRAP
		 * That is, we start at A. When we reach Z, we go to 0. At 9, we go to
		 * A again, in an iterative fashion.. so..
		 * AAA9 -> AABA, and so on. -- w00t
		 */

		/* start at the end of the current UID string, work backwards. don't trample on SID! */
		for (i = UUID_LENGTH - 2; i > 3; i--)
		{
			if (current_uid[i] == 'Z')
			{
				/* reached the end of alphabetical, go to numeric range */
				current_uid[i] = '0';
			}
			else if (current_uid[i] == '9')
			{
				/* we reached the end of the sequence, set back to A */
				current_uid[i] = 'A';

				/* we also need to increment the next digit. */
				continue;
			}
			else
			{
				/* most common case .. increment current UID */
				current_uid[i]++;
			}

			if (current_uid[3] == 'Z')
			{
				/*
				 * Ugh. We have run out of room.. roll back around to the
				 * start of the UUID namespace. -- w00t
				 */
				this->InitialiseUID();

				/*
				 * and now we need to break the inner for () to continue the while (),
				 * which will start the checking process over again. -- w00t
				 */
				break;
				
			}
			
			if (this->FindUUID(current_uid))
			{
				/*
				 * It's in use. We need to try the loop again.
				 */
				continue;
			}

			return current_uid;
		}
	}
#endif
	/* not reached. */
	return "";
}


