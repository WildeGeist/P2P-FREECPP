/*
 * Copyright (C) 2001-2011 Jacek Sieka, arnetheduck on gmail point com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "stdinc.h"
#include "SearchManager.h"

#include <boost/scoped_array.hpp>

#include "ClientManager.h"
#include "ConnectivityManager.h"
#include "format.h"
#include "LogManager.h"
#include "UploadManager.h"
#include "SearchResult.h"
#include "ShareManager.h"

namespace dcpp {

const char* SearchManager::types[TYPE_LAST] = {
	N_("Any"),
	N_("Audio"),
	N_("Compressed"),
	N_("Document"),
	N_("Executable"),
	N_("Picture"),
	N_("Video"),
	N_("Directory"),
	N_("TTH")
};
const char* SearchManager::getTypeStr(int type) {
	return _(types[type]);
}

SearchManager::SearchManager() :
	stop(false),
	lastSearch(GET_TICK())
{

}

SearchManager::~SearchManager() {
	if(socket.get()) {
		stop = true;
		socket->disconnect();
#ifdef _WIN32
		join();
#endif
	}
}

string SearchManager::normalizeWhitespace(const string& aString){
	string::size_type found = 0;
	string normalized = aString;
	while((found = normalized.find_first_of("\t\n\r", found)) != string::npos) {
		normalized[found] = ' ';
		found++;
	}
	return normalized;
}

void SearchManager::search(const string& aName, int64_t aSize, TypeModes aTypeMode /* = TYPE_ANY */, SizeModes aSizeMode /* = SIZE_ATLEAST */, const string& aToken /* = Util::emptyString */) {
	if(okToSearch()) {
		ClientManager::getInstance()->search(aSizeMode, aSize, aTypeMode, normalizeWhitespace(aName), aToken);
		lastSearch = GET_TICK();
	}
}

void SearchManager::search(StringList& who, const string& aName, int64_t aSize /* = 0 */, TypeModes aTypeMode /* = TYPE_ANY */, SizeModes aSizeMode /* = SIZE_ATLEAST */, const string& aToken /* = Util::emptyString */, const StringList& aExtList) {
	if(okToSearch()) {
		ClientManager::getInstance()->search(who, aSizeMode, aSize, aTypeMode, normalizeWhitespace(aName), aToken, aExtList);
		lastSearch = GET_TICK();
	}
}

void SearchManager::listen() {
	disconnect();

	try {
		socket.reset(new Socket(Socket::TYPE_UDP));
		socket->setLocalIp4(CONNSETTING(BIND_ADDRESS));
		socket->setLocalIp6(CONNSETTING(BIND_ADDRESS6));
		port = socket->listen(Util::toString(CONNSETTING(UDP_PORT)));
		start();
	} catch(...) {
		socket.reset();
		throw;
	}
}

void SearchManager::disconnect() noexcept {
	if(socket.get()) {
		stop = true;
		socket->disconnect();
		port.clear();

		join();

		socket.reset();

		stop = false;
	}
}

#define BUFSIZE 8192
int SearchManager::run() {
	boost::scoped_array<uint8_t> buf(new uint8_t[BUFSIZE]);
	int len;
	string remoteAddr;

	while(!stop) {
		try {
			if(!socket->wait(400, true, false).first) {
				continue;
			}

			if((len = socket->read(&buf[0], BUFSIZE, remoteAddr)) > 0) {
				onData(&buf[0], len, remoteAddr);
				continue;
			}
		} catch(const SocketException& e) {
			dcdebug("SearchManager::run Error: %s\n", e.getError().c_str());
		}

		bool failed = false;
		while(!stop) {
			try {
				socket->disconnect();
				port = socket->listen(Util::toString(CONNSETTING(UDP_PORT)));
				if(failed) {
					LogManager::getInstance()->message(_("Search enabled again"));
					failed = false;
				}
				break;
			} catch(const SocketException& e) {
				dcdebug("SearchManager::run Stopped listening: %s\n", e.getError().c_str());

				if(!failed) {
					LogManager::getInstance()->message(str(F_("Search disabled: %1%") % e.getError()));
					failed = true;
				}

				// Spin for 60 seconds
				for(int i = 0; i < 60 && !stop; ++i) {
					Thread::sleep(1000);
				}
			}
		}
	}
	return 0;
}

void SearchManager::onData(const uint8_t* buf, size_t aLen, const string& remoteIp) {
	string x((char*)buf, aLen);
	if(x.compare(0, 4, "$SR ") == 0) {
		string::size_type i, j;
		// Directories: $SR <nick><0x20><directory><0x20><free slots>/<total slots><0x05><Hubname><0x20>(<Hubip:port>)
		// Files:		$SR <nick><0x20><filename><0x05><filesize><0x20><free slots>/<total slots><0x05><Hubname><0x20>(<Hubip:port>)
		i = 4;
		if( (j = x.find(' ', i)) == string::npos) {
			return;
		}
		string nick = x.substr(i, j-i);
		i = j + 1;

		// A file has 2 0x05, a directory only one
		size_t cnt = count(x.begin() + j, x.end(), 0x05);

		SearchResult::Types type = SearchResult::TYPE_FILE;
		string file;
		int64_t size = 0;

		if(cnt == 1) {
			// We have a directory...find the first space beyond the first 0x05 from the back
			// (dirs might contain spaces as well...clever protocol, eh?)
			type = SearchResult::TYPE_DIRECTORY;
			// Get past the hubname that might contain spaces
			if((j = x.rfind(0x05)) == string::npos) {
				return;
			}
			// Find the end of the directory info
			if((j = x.rfind(' ', j-1)) == string::npos) {
				return;
			}
			if(j < i + 1) {
				return;
			}
			file = x.substr(i, j-i) + '\\';
		} else if(cnt == 2) {
			if( (j = x.find((char)5, i)) == string::npos) {
				return;
			}
			file = x.substr(i, j-i);
			i = j + 1;
			if( (j = x.find(' ', i)) == string::npos) {
				return;
			}
			size = Util::toInt64(x.substr(i, j-i));
		}
		i = j + 1;

		if( (j = x.find('/', i)) == string::npos) {
			return;
		}
		int freeSlots = Util::toInt(x.substr(i, j-i));
		i = j + 1;
		if( (j = x.find((char)5, i)) == string::npos) {
			return;
		}
		int slots = Util::toInt(x.substr(i, j-i));
		i = j + 1;
		if( (j = x.rfind(" (")) == string::npos) {
			return;
		}
		string hubName = x.substr(i, j-i);
		i = j + 2;
		if( (j = x.rfind(')')) == string::npos) {
			return;
		}

		string hubIpPort = x.substr(i, j-i);
		string url = ClientManager::getInstance()->findHub(hubIpPort);

		string encoding = ClientManager::getInstance()->findHubEncoding(url);
		nick = Text::toUtf8(nick, encoding);
		file = Text::toUtf8(file, encoding);
		hubName = Text::toUtf8(hubName, encoding);

		UserPtr user = ClientManager::getInstance()->findUser(nick, url);
		if(!user) {
			// Could happen if hub has multiple URLs / IPs
			user = ClientManager::getInstance()->findLegacyUser(nick);
			if(!user)
				return;
		}

		string tth;
		if(hubName.compare(0, 4, "TTH:") == 0) {
			tth = hubName.substr(4);
			StringList names = ClientManager::getInstance()->getHubNames(user->getCID(), Util::emptyString);
			hubName = names.empty() ? _("Offline") : Util::toString(names);
		}

		if(tth.empty() && type == SearchResult::TYPE_FILE) {
			return;
		}


		SearchResultPtr sr(new SearchResult(user, type, slots, freeSlots, size,
			file, hubName, url, remoteIp, TTHValue(tth), Util::emptyString));
		fire(SearchManagerListener::SR(), sr);

	} else if(x.compare(1, 4, "RES ") == 0 && x[x.length() - 1] == 0x0a) {
		AdcCommand c(x.substr(0, x.length()-1));
		if(c.getParameters().empty())
			return;
		string cid = c.getParam(0);
		if(cid.size() != 39)
			return;

		UserPtr user = ClientManager::getInstance()->findUser(CID(cid));
		if(!user)
			return;

		// This should be handled by AdcCommand really...
		c.getParameters().erase(c.getParameters().begin());

		onRES(c, user, remoteIp);

	} /*else if(x.compare(1, 4, "SCH ") == 0 && x[x.length() - 1] == 0x0a) {
		try {
			respond(AdcCommand(x.substr(0, x.length()-1)));
		} catch(ParseException& ) {
		}
	}*/ // Needs further DoS investigation
}

void SearchManager::onRES(const AdcCommand& cmd, const UserPtr& from, const string& remoteIp) {
	int freeSlots = -1;
	int64_t size = -1;
	string file;
	string tth;
	string token;

	for(StringIterC i = cmd.getParameters().begin(); i != cmd.getParameters().end(); ++i) {
		const string& str = *i;
		if(str.compare(0, 2, "FN") == 0) {
			file = Util::toNmdcFile(str.substr(2));
		} else if(str.compare(0, 2, "SL") == 0) {
			freeSlots = Util::toInt(str.substr(2));
		} else if(str.compare(0, 2, "SI") == 0) {
			size = Util::toInt64(str.substr(2));
		} else if(str.compare(0, 2, "TR") == 0) {
			tth = str.substr(2);
		} else if(str.compare(0, 2, "TO") == 0) {
			token = str.substr(2);
		}
	}

	if(!file.empty() && freeSlots != -1 && size != -1) {

		/// @todo get the hub this was sent from, to be passed as a hint? (eg by using the token?)
		StringList names = ClientManager::getInstance()->getHubNames(from->getCID());
		string hubName = names.empty() ? _("Offline") : Util::toString(names);
		StringList hubs = ClientManager::getInstance()->getHubUrls(from->getCID());
		string hub = hubs.empty() ? _("Offline") : Util::toString(hubs);

		SearchResult::Types type = (file[file.length() - 1] == '\\' ? SearchResult::TYPE_DIRECTORY : SearchResult::TYPE_FILE);
		if(type == SearchResult::TYPE_FILE && tth.empty())
			return;
		/// @todo Something about the slots
		SearchResultPtr sr(new SearchResult(from, type, 0, freeSlots, size,
			file, hubName, hub, remoteIp, TTHValue(tth), token));
		fire(SearchManagerListener::SR(), sr);
	}
}

void SearchManager::respond(const AdcCommand& adc, const CID& from,  bool isUdpActive) {
	// Filter own searches
	if(from == ClientManager::getInstance()->getMe()->getCID())
		return;

	UserPtr p = ClientManager::getInstance()->findUser(from);
	if(!p)
		return;

	SearchResultList results;
	ShareManager::getInstance()->search(results, adc.getParameters(), isUdpActive ? 10 : 5);

	string token;

	adc.getParam("TO", 0, token);

	if(results.empty())
		return;

	for(SearchResultList::const_iterator i = results.begin(); i != results.end(); ++i) {
		AdcCommand cmd = (*i)->toRES(AdcCommand::TYPE_UDP);
		if(!token.empty())
			cmd.addParam("TO", token);
		ClientManager::getInstance()->send(cmd, from);
	}
}

} // namespace dcpp
