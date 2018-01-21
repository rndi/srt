/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2017 Haivision Systems Inc.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; If not, see <http://www.gnu.org/licenses/>
 */

// NOTE: This application uses C++11.

// This program uses quite a simple architecture, which is mainly related to
// the way how it's invoked: stransmit <source> <target> (plus options).
//
// The media for <source> and <target> are filled by abstract classes
// named Source and Target respectively. Most important virtuals to
// be filled by the derived classes are Source::Read and Target::Write.
//
// For SRT please take a look at the SrtCommon class first. This contains
// everything that is needed for creating an SRT medium, that is, making
// a connection as listener, as caller, and as rendezvous. The listener
// and caller modes are built upon the same philosophy as those for
// BSD/POSIX socket API (bind/listen/accept or connect).
//
// The instance class is selected per details in the URI (usually scheme)
// and then this URI is used to configure the medium object. Medium-specific
// options are specified in the URI: SCHEME://HOST:PORT?opt1=val1&opt2=val2 etc.
//
// Options for connection are set by ConfigurePre and ConfigurePost.
// This is a philosophy that exists also in BSD/POSIX sockets, just not
// officially mentioned:
// - The "PRE" options must be set prior to connecting and can't be altered
//   on a connected socket, however if set on a listening socket, they are
//   derived by accept-ed socket. 
// - The "POST" options can be altered any time on a connected socket.
//   They MAY have also some meaning when set prior to connecting; such
//   option is SRTO_RCVSYN, which makes connect/accept call asynchronous.
//   Because of that this option is treated special way in this app.
//
// See 'srt_options' global variable (common/socketoptions.hpp) for a list of
// all options.

// MSVS likes to complain about lots of standard C functions being unsafe.
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#define REQUIRE_CXX11 1

#include <cctype>
#include <iostream>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <memory>
#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <cstring>
#include <csignal>
#include <chrono>
#include <thread>
#include <list>

#include "../common/appcommon.hpp"  // CreateAddrInet
#include "../common/uriparser.hpp"  // UriParser
#include "../common/socketoptions.hpp"
#include "../common/logsupport.hpp"
#include "../common/transmitbase.hpp"

// NOTE: This is without "haisrt/" because it uses an internal path
// to the library. Application using the "installed" library should
// use <srt/srt.h>
#include <srt.h>
#include <logging.h>

using namespace std;

map<string,string> g_options;

string Option(string deflt="") { return deflt; }

template <class... Args>
string Option(string deflt, string key, Args... further_keys)
{
    map<string, string>::iterator i = g_options.find(key);
    if ( i == g_options.end() )
        return Option(deflt, further_keys...);
    return i->second;
}

ostream* cverb = &cout;

struct ForcedExit: public std::runtime_error
{
    ForcedExit(const std::string& arg):
        std::runtime_error(arg)
    {
    }
};

struct AlarmExit: public std::runtime_error
{
    AlarmExit(const std::string& arg):
        std::runtime_error(arg)
    {
    }
};

volatile bool int_state = false;
volatile bool timer_state = false;
void OnINT_ForceExit(int)
{
    if (transmit_verbose)
    {
        cerr << "\n-------- REQUESTED INTERRUPT!\n";
    }

    int_state = true;
}

void OnAlarm_Interrupt(int)
{
    if (transmit_verbose)
    {
        cerr << "\n---------- INTERRUPT ON TIMEOUT!\n";
    }

    int_state = false; // JIC
    timer_state = true;

    if ((false))
    {
        throw AlarmExit("Watchdog bites hangup");
    }
}

struct BandwidthGuard
{
    typedef std::chrono::steady_clock::time_point time_point;
    size_t conf_bw;
    time_point start_time, prev_time;
    size_t report_count = 0;
    double average_bw = 0;
    size_t transfer_size = 0;

    BandwidthGuard(size_t band): conf_bw(band), start_time(std::chrono::steady_clock::now()), prev_time(start_time) {}

    void Checkpoint(size_t size, size_t toreport )
    {
        using namespace std::chrono;

        time_point eop = steady_clock::now();
        auto dur = duration_cast<microseconds>(eop - start_time);
        //auto this_dur = duration_cast<microseconds>(eop - prev_time);

        transfer_size += size;
        average_bw = transfer_size*1000000.0/dur.count();
        //double this_bw = size*1000000.0/this_dur.count();

        if ( toreport )
        {
            // Show current bandwidth
            ++report_count;
            if ( report_count % toreport == toreport - 1 )
            {
                cout.precision(10);
                int abw = int(average_bw);
                int abw_trunc = abw/1024;
                int abw_frac = abw%1024;
                char bufbw[64];
                sprintf(bufbw, "%d.%03d", abw_trunc, abw_frac);
                cout << "+++/+++SRT TRANSFER: " << transfer_size << "B "
                    "DURATION: "  << duration_cast<milliseconds>(dur).count() << "ms SPEED: " << bufbw << "kB/s\n";
            }
        }

        prev_time = eop;

        if ( transfer_size > SIZE_MAX/2 )
        {
            transfer_size -= SIZE_MAX/2;
            start_time = eop;
        }

        if ( conf_bw == 0 )
            return; // don't guard anything

        // Calculate expected duration for the given size of bytes (in [ms])
        double expdur_ms = double(transfer_size)/conf_bw*1000;

        auto expdur = milliseconds(size_t(expdur_ms));
        // Now compare which is more

        if ( dur >= expdur ) // too slow, but there's nothing we can do. Exit now.
            return;

        std::this_thread::sleep_for(expdur-dur);
    }
};

extern "C" void TestLogHandler(void* opaque, int level, const char* file, int line, const char* area, const char* message);

int main( int argc, char** argv )
{
    // This is mainly required on Windows to initialize the network system,
    // for a case when the instance would use UDP. SRT does it on its own, independently.
    if ( !SysInitializeNetwork() )
        throw std::runtime_error("Can't initialize network!");

    // Symmetrically, this does a cleanup; put into a local destructor to ensure that
    // it's called regardless of how this function returns.
    struct NetworkCleanup
    {
        ~NetworkCleanup()
        {
            SysCleanupNetwork();
        }
    } cleanupobj;

    vector<string> args;
    copy(argv+1, argv+argc, back_inserter(args));

    // Check options
    vector<string> params;

    for (string a: args)
    {
        if ( a[0] == '-' )
        {
            string key = a.substr(1);
            size_t pos = key.find(':');
            if ( pos == string::npos )
                pos = key.find(' ');
            string value = pos == string::npos ? "" : key.substr(pos+1);
            key = key.substr(0, pos);
            g_options[key] = value;
            continue;
        }

        params.push_back(a);
    }

    if ( params.size() != 2 )
    {
        cerr << "Usage: " << argv[0] << " [options] <input-uri> <output-uri>\n";
        cerr << "\t-t:<timeout=0> - exit timer in seconds\n";
        cerr << "\t-c:<chunk=1316> - max size of data read in one step\n";
        cerr << "\t-b:<bandwidth> - set SRT bandwidth\n";
        cerr << "\t-r:<report-frequency=0> - bandwidth report frequency\n";
        cerr << "\t-s:<stats-report-freq=0> - frequency of status report\n";
#if 0
        cerr << "\t-k - crash on error (aka developer mode)\n";
#endif
        cerr << "\t-q - quiet mode, default no\n";
        cerr << "\t-v - verbose mode, default no\n";
        cerr << "\t-a - auto-reconnect mode, default yes, -a:no to disable\n";
        return 1;
    }

    int timeout = stoi(Option("0", "t", "to", "timeout"), 0, 0);
    size_t chunk = stoul(Option("0", "c", "chunk"), 0, 0);
    if ( chunk == 0 )
    {
        chunk = SRT_LIVE_DEF_PLSIZE;
    }
    else
    {
        transmit_chunk_size = chunk;
    }

    transmit_verbose = Option("no", "v", "verbose") != "no";
#if 0
    bool crashonx = Option("no", "k", "crash") != "no";
#endif
    string loglevel = Option("error", "loglevel");
    string logfa = Option("general", "logfa");
    string logfile = Option("", "logfile");
    bool internal_log = Option("no", "loginternal") != "no";
    bool autoreconnect = Option("yes", "a", "auto") != "no";
    bool quiet = Option("no", "q", "quiet") != "no";
#if 0
    bool skip_flushing = Option("no", "S", "skipflush") != "no";
#endif
    // Options that require integer conversion
#if 0
    size_t stoptime;
    size_t bandwidth;
#endif
    try
    {
        transmit_bw_report = stoul(Option("0", "r", "report", "bandwidth-report", "bitrate-report"));
        transmit_stats_report = stoi(Option("0", "s", "stats", "stats-report-frequency"));
#if 0
        bandwidth = stoul(Option("0", "b", "bandwidth", "bitrate"));
        stoptime = stoul(Option("0", "d", "stoptime"));
#endif
    }
    catch (std::invalid_argument)
    {
        cerr << "ERROR: Incorrect integer number specified for an option.\n";
        return 1;
    }

    std::ofstream logfile_stream; // leave unused if not set

    srt_setloglevel(SrtParseLogLevel(loglevel));
    set<logging::LogFA> fas = SrtParseLogFA(logfa);
    for (set<logging::LogFA>::iterator i = fas.begin(); i != fas.end(); ++i)
        srt_addlogfa(*i);

    char NAME[] = "SRTLIB";
    if ( internal_log )
    {
        srt_setlogflags( 0
                | SRT_LOGF_DISABLE_TIME
                | SRT_LOGF_DISABLE_SEVERITY
                | SRT_LOGF_DISABLE_THREADNAME
                | SRT_LOGF_DISABLE_EOL
                );
        srt_setloghandler(NAME, TestLogHandler);
    }
    else if ( logfile != "" )
    {
        logfile_stream.open(logfile.c_str());
        if ( !logfile_stream )
        {
            cerr << "ERROR: Can't open '" << logfile << "' for writing - fallback to cerr\n";
        }
        else
        {
            UDT::setlogstream(logfile_stream);
        }
    }


#ifdef WIN32
#define alarm(argument) (void)0

    if (stoptime != 0)
    {
        cerr << "ERROR: The -stoptime option (-d) is not implemented on Windows\n";
        return 1;
    }

#else
    siginterrupt(SIGALRM, true);
    signal(SIGALRM, OnAlarm_Interrupt);
#endif
    siginterrupt(SIGINT, true);
    signal(SIGINT, OnINT_ForceExit);
    siginterrupt(SIGINT, true);
    signal(SIGTERM, OnINT_ForceExit);

    if (timeout != 0)
    {
        if (!quiet)
            cerr << "TIMEOUT: will interrupt after " << timeout << "s\n";
        alarm(timeout);
    }

#if 0
        BandwidthGuard bw(bandwidth);
#endif


    if (!quiet)
    {
        cout << "Media path: '"
            << params[0]
            << "' --> '"
            << params[1]
            << "'\n";
    }

    unique_ptr<Source> src;
    bool srcConnected = false;
    unique_ptr<Target> tar;
    bool tarConnected = false;

    extern logging::Logger glog;
    int pollid = srt_epoll_create();
    if ( pollid < 0 )
    {
        cerr << "Can't initialize epoll";
        return 1;
    }

    try {
        // Now loop until broken
        while (!int_state && !timer_state)
        {
            if (!src.get())
            {
                src = Source::Create(params[0]);
                if (!src.get())
                {
                    cerr << "Unsupported source type" << endl;
                    return 1;
                }
                int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
                switch(src->uri.type())
                {
                case UriParser::SRT:
                    if (srt_epoll_add_usock(pollid,
                            src->GetSRTSocket(), &events))
                    {
                        cerr << "Failed to add SRT source to poll, "
                            << src->GetSRTSocket() << endl;
                        return 1;
                    }
                    break;
                case UriParser::UDP:
                    if (srt_epoll_add_ssock(pollid,
                            src->GetSysSocket(), &events))
                    {
                        cerr << "Failed to add UDP source to poll, "
                            << src->GetSysSocket() << endl;
                        return 1;
                    }
                    break;
                case UriParser::FILE:
                    if (srt_epoll_add_ssock(pollid,
                            src->GetSysSocket(), &events))
                    {
                        cerr << "Failed to add UDP source to poll, "
                            << src->GetSysSocket() << endl;
                        return 1;
                    }
                    break;
                default:
                    break;
                }
            }

            if (!tar.get())
            {
                tar = Target::Create(params[1]);
                if (!src.get())
                {
                    cerr << "Unsupported target type" << endl;
                    return 1;
                }

                // IN because we care for state transitions only
                int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
                switch(tar->uri.type())
                {
                case UriParser::SRT:
                    if (srt_epoll_add_usock(pollid,
                            tar->GetSRTSocket(), &events))
                    {
                        cerr << "Failed to add SRT destination to poll, "
                            << tar->GetSRTSocket() << endl;
                        return 1;
                    }
                    break;
                default:
                    break;
                }
            }

            int srtrfdslen = 2;
            SRTSOCKET srtrfds[2];
            int sysrfdslen = 2;
            int sysrfds[2];
            if (srt_epoll_wait(pollid,
                srtrfds, &srtrfdslen, 0, 0,
                100,
                sysrfds, &sysrfdslen, 0, 0) >= 0)
            {
                if ((false))
                {
                    cout << "Event:"
                        << " srtrfdslen " << srtrfdslen
                        << " sysrfdslen " << sysrfdslen
                        << endl;
                }

                bool doabort = false;
                for (int i = 0; i < srtrfdslen; i++)
                {
                    bool issource = false;
                    SRTSOCKET s = srtrfds[i];
                    if (src->GetSRTSocket() == s)
                    {
                        issource = true;
                    }
                    else if (tar->GetSRTSocket() != s)
                    {
                        cerr << "Unexpected socket poll: " << s;
                        doabort = true;
                        break;
                    }

                    const char * dirstring = (issource)? "source" : "target";

                    SRT_SOCKSTATUS status = srt_getsockstate(s);
                    if ((false) && status != CONNECTED)
                    {
                        cout << dirstring << " status " << status << endl;
                    }
                    switch (status)
                    {
                        case LISTENING:
                        {
                            if ((false) && !quiet)
                                cout << "New SRT client connection" << endl;

                            bool res = (issource) ?
                                src->AcceptNewClient() : tar->AcceptNewClient();
                            if (!res)
                            {
                                cerr << "Failed to accept SRT connection"
                                    << endl;
                                doabort = true;
                                break;
                            }

                            srt_epoll_remove_usock(pollid, res);

                            SRTSOCKET ns = (issource) ?
                                src->GetSRTSocket() : tar->GetSRTSocket();
                            int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
                            if (srt_epoll_add_usock(pollid, ns, &events))
                            {
                                cerr << "Failed to add SRT client to poll, "
                                    << ns << endl;
                                doabort = true;
                            }
                            else if (!quiet)
                            {
                                cout << "Accepting SRT "
                                    << dirstring
                                    <<  " connection"
                                    << endl;
                            }
                        }
                        break;
                        case BROKEN:
                        case NONEXIST:
                        case CLOSED:
                        {
                            if (issource)
                            {
                                if (srcConnected)
                                {
                                    if (!quiet)
                                    {
                                        cout << "SRT source disconnected"
                                            << endl;
                                    }
                                    srcConnected = false;
                                }
                            }
                            else if (!tarConnected)
                            {
                                if (!quiet)
                                    cout << "SRT target disconnected" << endl;
                                tarConnected = false;
                            }

                            if(!autoreconnect)
                            {
                                doabort = true;
                            }
                            else
                            {
                                // force re-connection
                                srt_epoll_remove_usock(pollid, s);
                                if (issource)
                                    src.release();
                                else
                                    tar.release();
                            }
                        }
                        break;
                        case CONNECTED:
                        {
                            if (issource)
                            {
                                if (!srcConnected)
                                {
                                    if (!quiet)
                                        cout << "SRT source connected" << endl;
                                    srcConnected = true;
                                }
                            }
                            else if (!tarConnected)
                            {
                                if (!quiet)
                                    cout << "SRT target connected" << endl;
                                tarConnected = true;
                            }
                        }

                        default:
                        {
                            // No-Op
                        }
                        break;
                    }
                }

                if (doabort)
                {
                    break;
                }

                // read a few chunks at a time in attempt to deplete
                // read buffers as much as possible on each read event
                // note that this implies live streams and does not
                // work for cached/file sources
                std::list<std::shared_ptr<bytevector>> dataqueue;
                if (src.get() && (srtrfdslen || sysrfdslen))
                {
                    while (dataqueue.size() < 10)
                    {
                        std::shared_ptr<bytevector> pdata(
                            new bytevector(chunk));
                        if (!src->Read(chunk, *pdata) || (*pdata).empty())
                        {
                            break;
                        }
                        dataqueue.push_back(pdata);
                    }
                }

                // if no target, let received data fall to the floor
                while (tar.get() && !dataqueue.empty())
                {
                    std::shared_ptr<bytevector> pdata = dataqueue.front();
                    if (tar->IsOpen())
                    {
                        tar->Write(*pdata);
                    }
                    dataqueue.pop_front();
                }
            }
        }
    }
    catch (std::exception& x)
    {
        cerr << "ERROR: " << x.what() << endl;
        return 255;
    }

    return 0;
}

// Class utilities


void TestLogHandler(void* opaque, int level, const char* file, int line, const char* area, const char* message)
{
    char prefix[100] = "";
    if ( opaque )
        strncpy(prefix, (char*)opaque, 99);
    time_t now;
    time(&now);
    char buf[1024];
    struct tm local = LocalTime(now);
    size_t pos = strftime(buf, 1024, "[%c ", &local);

#ifdef _MSC_VER
    // That's something weird that happens on Microsoft Visual Studio 2013
    // Trying to keep portability, while every version of MSVS is a different plaform.
    // On MSVS 2015 there's already a standard-compliant snprintf, whereas _snprintf
    // is available on backward compatibility and it doesn't work exactly the same way.
#define snprintf _snprintf
#endif
    snprintf(buf+pos, 1024-pos, "%s:%d(%s)]{%d} %s", file, line, area, level, message);

    cerr << buf << endl;
}

