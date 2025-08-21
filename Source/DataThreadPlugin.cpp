/*
 ------------------------------------------------------------------

 This file is part of the Open Ephys GUI
 Copyright (C) 2022 Open Ephys

 ------------------------------------------------------------------

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

 */

#include "DataThreadPlugin.h"
#include "DataThreadPluginEditor.h"

// Server Stuff
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <iostream>
#include <string_view>

const int NUM_CHANNELS = 16;
const int MAX_SAMPLES_PER_CHANNEL = 1024;
const int MAX_SAMPLES_PER_BUFFER = MAX_SAMPLES_PER_CHANNEL;
int raw_samples[NUM_CHANNELS * MAX_SAMPLES_PER_CHANNEL];
float scaled_samples[NUM_CHANNELS * MAX_SAMPLES_PER_BUFFER];
int64 sample_numbers[MAX_SAMPLES_PER_CHANNEL];
uint64 event_codes[MAX_SAMPLES_PER_CHANNEL];
double timestamps[MAX_SAMPLES_PER_CHANNEL];
int64 totalSamples = 0;
DataBuffer* dataBuffer;

// UDP variables
int port = 8080;

std::array<char, 65536> buf{}; // max UDP payload size (practical)

constexpr int MAX_EVENTS = 64;
std::array<epoll_event, MAX_EVENTS> events;

// UDP stuff idk
int sock; // UDP socket
int sfd;
int ep;

int server_running = false;

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int init_udp_server() {
    // Create UDP socket (IPv4)
    sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == -1) {
        LOGD("socket");
        return 1;
    }

    // Allow quick rebinding (useful during dev)
    int yes = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
         LOGD("setsockopt(SO_REUSEADDR)");
        return 1;
    }

    if (set_nonblocking(sock) == -1) {
        LOGD("fcntl(O_NONBLOCK)");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
        perror("bind");
        return 1;
    }

    // Create signalfd so we can shut down cleanly via epoll (Ctrl+C)
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    // Block signals from default handling
    if (pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        LOGD("pthread_sigmask");
        return 1;
    }
    sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd == -1) {
        LOGD("signalfd");
        return 1;
    }

    // epoll setup
    ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep == -1) {
        LOGD("epoll_create1");
        return 1;
    }

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;   // edge-triggered, read events
    ev.data.fd = sock;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, sock, &ev) == -1) {
        LOGD("epoll_ctl(ADD sock)");
        return 1;
    }

    epoll_event sig_ev{};
    sig_ev.events = EPOLLIN;
    sig_ev.data.fd = sfd;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &sig_ev) == -1) {
        LOGD("epoll_ctl(ADD signalfd)");
        return 1;
    }

    LOGD("UDP server listening on port ");
	server_running = true;

	return 0;
    
}

int close_udp_server() {
	close(ep);
    close(sfd);
    close(sock);
    return 0;
}

int tick_udp()
{
	int n = epoll_wait(ep, events.data(), MAX_EVENTS, -1);
	if (n == -1) {
		if (errno == EINTR) return 1;
		LOGD("epoll_wait");
		return 1;
	}

	for (int i = 0; i < n; ++i) {
		int fd = events[i].data.fd;

		if (fd == sfd) {
			// Handle shutdown signal
			signalfd_siginfo si;
			ssize_t r = read(sfd, &si, sizeof(si));
			(void)r;
			server_running = false;
			break;
		}

		if (fd == sock) {
			// Drain all readable datagrams (edge-triggered!)
			while (true) {
				sockaddr_in src{};
				socklen_t srclen = sizeof(src);
				ssize_t r = recvfrom(sock, buf.data(), buf.size(), 0,
									 reinterpret_cast<sockaddr*>(&src), &srclen);
				if (r > 0) {
					LOGD("Got %d btyes ", r);
					// Example "processing": print and echo back
					char ip[INET_ADDRSTRLEN];
					inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
					uint16_t sport = ntohs(src.sin_port);

					std::string_view msg(buf.data(), static_cast<size_t>(r));
					// std::cout << "Got " << r << " bytes from " << ip << ":" << sport
					//		  << " -> \"" << msg << "\"\n";

					LOGD("Got msg %s", msg);

					// Optional: echo response
					// sendto(sock, msg.data(), msg.size(), 0,
					//        reinterpret_cast<sockaddr*>(&src), srclen);
				} else if (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
					// No more packets
					break;
				} else if (r == 0) {
					// UDP doesn't really give 0 here, but handle defensively
					break;
				} else {
					LOGD("recvfrom");
					break;
				}
			}
		}
	}

}


struct PluginSettingsObject
{
    // Store settings for the plugin here
};

DataThreadPlugin::DataThreadPlugin (SourceNode* sn) : DataThread (sn)
{
}

DataThreadPlugin::~DataThreadPlugin()
{
}

bool DataThreadPlugin::foundInputSource()
{
    return true;
}

void DataThreadPlugin::updateSettings (OwnedArray<ContinuousChannel>* continuousChannels,
                                       OwnedArray<EventChannel>* eventChannels,
                                       OwnedArray<SpikeChannel>* spikeChannels,
                                       OwnedArray<DataStream>* sourceStreams,
                                       OwnedArray<DeviceInfo>* devices,
                                       OwnedArray<ConfigurationObject>* configurationObjects)
{
	// Clear previous values
   sourceStreams->clear();
   sourceBuffers.clear(); // DataThread class member
   continuousChannels->clear();
   eventChannels->clear();

   DataStream::Settings settings
   {
      "device_stream", // stream name
      "description",   // stream description
      "identifier",    // stream identifier
      30000.0          // stream sample rate
   };

   DataStream* stream = new DataStream(settings);

   sourceStreams->add(stream); // add pointer to owned array

   // create a data buffer and add it to the sourceBuffer array
   sourceBuffers.add(new DataBuffer(NUM_CHANNELS, 48000));
   dataBuffer = sourceBuffers.getLast();

   for (int i = 0; i < NUM_CHANNELS; i++)
   {
      ContinuousChannel::Settings settings{
                             ContinuousChannel::Type::ELECTRODE, // channel type
                             "CH" + String(i+1), // channel name
                             "description",      // channel description
                             "identifier",       // channel identifier
                             0.195,              // channel bitvolts scaling
                             stream              // associated data stream
                     };

      continuousChannels->add(new ContinuousChannel(settings));
   }

   EventChannel::Settings settings2{
                     EventChannel::Type::TTL, // channel type (must be TTL)
                     "Device Event Channel",  // channel name
                     "description",           // channel description
                     "identifier",            // channel identifier
                     stream,                  // associated data stream
                     8                        // maximum number of TTL lines
             };

   eventChannels->add(new EventChannel(settings2));
}

bool DataThreadPlugin::startAcquisition()
{
	startThread();
	init_udp_server(); 
	return true;
}



bool DataThreadPlugin::updateBuffer()
{


   for (int i = 0; i < MAX_SAMPLES_PER_CHANNEL; i++)
   {
      for (int j = 0; j < NUM_CHANNELS; j++)
      {
         scaled_samples[i + j * MAX_SAMPLES_PER_CHANNEL] = (float) (j + i * NUM_CHANNELS);
      }
      sample_numbers[i] = totalSamples++;
   }

	tick_udp();

   dataBuffer->addToBuffer(scaled_samples,
                           sample_numbers,
                           timestamps,
                           event_codes,
                           MAX_SAMPLES_PER_CHANNEL);

   return true;

}

bool DataThreadPlugin::stopAcquisition()
{
	if (isThreadRunning())
	{
	  signalThreadShouldExit(); //stop thread
	}

	waitForThreadToExit(500);
	dataBuffer->clear();

	return true;
}

void DataThreadPlugin::resizeBuffers()
{
}

std::unique_ptr<GenericEditor> DataThreadPlugin::createEditor (SourceNode* sn)
{
    std::unique_ptr<DataThreadPluginEditor> editor = std::make_unique<DataThreadPluginEditor> (sn, this);

    return editor;
}

void DataThreadPlugin::handleBroadcastMessage (const String& msg, const int64 messageTimestmpMilliseconds)
{
}

String DataThreadPlugin::handleConfigMessage (const String& msg)
{
    return "";
}

void DataThreadPlugin::parameterValueChanged (Parameter* param)
{
   if (param->getName().equalsIgnoreCase ("port"))
   {
      LOGD ("Port changed"); // log message
   }
   else if (param->getName().equalsIgnoreCase ("interval"))
   {
   }
   else if (param->getName().equalsIgnoreCase ("output_line"))
   {
   }
}

void DataThreadPlugin::registerParameters()
{
   // Parameter for event frequency (Hz)
   addIntParameter (Parameter::PROCESSOR_SCOPE, // parameter scope
                     "port", // parameter name
                     "Port", // display name
                     "Port to listen for UDP packets", // parameter description
                     8080, // default value
                     0, // minimum value
                     65535, // maximum value
                     false); 
}


