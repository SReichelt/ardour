/*
    Copyright (C) 2012 Paul Davis 
    Inspired by code from Ben Loftis @ Harrison Consoles

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include <string>
#include <iostream>
#include <fstream>
#include <cstring>

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#include <glibmm.h>
#else
#include <sys/utsname.h>
#endif

#include <curl/curl.h>

#include <glibmm/miscutils.h>

#include "pbd/compose.h"
#include "pbd/pthread_utils.h"

#include "ardour/filesystem_paths.h"
#include "ardour/rc_configuration.h"

#include "pingback.h"

using std::string;
using namespace ARDOUR;

static size_t
curl_write_data (char *bufptr, size_t size, size_t nitems, void *ptr)
{
        /* we know its a string */

        string* sptr = (string*) ptr;

        for (size_t i = 0; i < nitems; ++i) {
                for (size_t n = 0; n < size; ++n) {
                        if (*bufptr == '\n') {
                                break;
                        }

                        (*sptr) += *bufptr++;
                }
        }

        return size * nitems;
}

struct ping_call {
    std::string version;
    std::string announce_path;

    ping_call (const std::string& v, const std::string& a)
	    : version (v), announce_path (a) {}
};

#ifdef PLATFORM_WINDOWS
static bool
_query_registry (const char *regkey, const char *regval, std::string &rv) {
	HKEY key;
	DWORD size = PATH_MAX;
	char tmp[PATH_MAX+1];

	if (   (ERROR_SUCCESS == RegOpenKeyExA (HKEY_LOCAL_MACHINE, regkey, 0, KEY_READ, &key))
	    && (ERROR_SUCCESS == RegQueryValueExA (key, regval, 0, NULL, reinterpret_cast<LPBYTE>(tmp), &size))
		 )
	{
		rv = Glib::locale_to_utf8 (tmp);
		return true;
	}

	if (   (ERROR_SUCCESS == RegOpenKeyExA (HKEY_LOCAL_MACHINE, regkey, 0, KEY_READ | KEY_WOW64_32KEY, &key))
	    && (ERROR_SUCCESS == RegQueryValueExA (key, regval, 0, NULL, reinterpret_cast<LPBYTE>(tmp), &size))
		 )
	{
		rv = Glib::locale_to_utf8 (tmp);
		return true;
	}

	return false;
}
#endif


static void*
_pingback (void *arg)
{
	ping_call* cm = static_cast<ping_call*> (arg);
	CURL* c;
	string return_str;
	//initialize curl

	curl_global_init (CURL_GLOBAL_NOTHING);
	c = curl_easy_init ();
	
	curl_easy_setopt (c, CURLOPT_WRITEFUNCTION, curl_write_data); 
	curl_easy_setopt (c, CURLOPT_WRITEDATA, &return_str); 
	char errbuf[CURL_ERROR_SIZE];
	curl_easy_setopt (c, CURLOPT_ERRORBUFFER, errbuf); 

	string url;

#ifdef __APPLE__
	url = Config->get_osx_pingback_url ();
#elif defined PLATFORM_WINDOWS
	url = Config->get_windows_pingback_url ();
#else
	url = Config->get_linux_pingback_url ();
#endif

	if (url.compare (0, 4, "http") != 0) {
		delete cm;
		return 0;
	}

	char* v = curl_easy_escape (c, cm->version.c_str(), cm->version.length());
	url += v;
	url += '?';
	free (v);

#ifndef PLATFORM_WINDOWS
	struct utsname utb;

	if (uname (&utb)) {
		delete cm;
		return 0;
	}

	//string uts = string_compose ("%1 %2 %3 %4", utb.sysname, utb.release, utb.version, utb.machine);
	string s;
	char* query;

	query = curl_easy_escape (c, utb.sysname, strlen (utb.sysname));
	s = string_compose ("s=%1", query);
	url += s;
	url += '&';
	free (query);

	query = curl_easy_escape (c, utb.release, strlen (utb.release));
	s = string_compose ("r=%1", query);
	url += s;
	url += '&';
	free (query);

	query = curl_easy_escape (c, utb.machine, strlen (utb.machine));
	s = string_compose ("m=%1", query);
	url += s;
	free (query);
#else
	std::string val;
	if (_query_registry("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName", val)) {
		char* query = curl_easy_escape (c, val.c_str(), strlen (val.c_str()));
		url += "r=";
		url += query;
		url += '&';
		free (query);
	} else {
		url += "r=&";
	}

	if (_query_registry("Hardware\\Description\\System\\CentralProcessor\\0", "Identifier", val)) {
		// remove "Family X Model YY Stepping Z" tail
		size_t cut = val.find (" Family ");
		if (string::npos != cut) {
			val = val.substr (0, cut);
		}
		char* query = curl_easy_escape (c, val.c_str(), strlen (val.c_str()));
		url += "m=";
		url += query;
		url += '&';
		free (query);
	} else {
		url += "m=&";
	}

# if ( defined(__x86_64__) || defined(_M_X64) )
	url += "s=Windows64";
# else
	url += "s=Windows32";
# endif

#endif /* PLATFORM_WINDOWS */

	curl_easy_setopt (c, CURLOPT_URL, url.c_str());

	return_str = "";

	if (curl_easy_perform (c) == 0) {
		long http_status; 

		curl_easy_getinfo (c, CURLINFO_RESPONSE_CODE, &http_status);

		if (http_status != 200) {
			std::cerr << "Bad HTTP status" << std::endl;
			return 0;
		}

		if ( return_str.length() > 140 ) { // like a tweet :)
			std::cerr << "Announcement string is too long (probably behind a proxy)." << std::endl;
		} else {
			std::cerr << "Announcement is: " << return_str << std::endl;
			
			//write announcements to local file, even if the
			//announcement is empty
				
			std::ofstream annc_file (cm->announce_path.c_str());
			
			if (annc_file) {
				annc_file << return_str;
			}
		}
	} else {
		std::cerr << "curl failed: " << errbuf << std::endl;
	}

	curl_easy_cleanup (c);
	delete cm;
	return 0;
}

namespace ARDOUR {

void pingback (const string& version, const string& announce_path) 
{
	ping_call* cm = new ping_call (version, announce_path);
	pthread_t thread;

	pthread_create_and_store ("pingback", &thread, _pingback, cm);
}

}
