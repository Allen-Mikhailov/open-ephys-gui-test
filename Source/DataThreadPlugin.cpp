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
#include <thread>
#include <chrono>

const int MAX_DATA_CHANNELS = 128;
const int METRICS_CHANNELS = 2;
const int MAX_SAMPLES_PER_CHANNEL = 1024;

int64 totalSamples = 0;

// Datapoints
float data_points[MAX_DATA_CHANNELS * MAX_SAMPLES_PER_CHANNEL];
int64 sample_numbers[MAX_SAMPLES_PER_CHANNEL]; 
uint64 event_codes[MAX_SAMPLES_PER_CHANNEL];
double timestamps[MAX_SAMPLES_PER_CHANNEL];



DataBuffer* dataBuffer;

// Metrics
float metric_data_points[METRICS_CHANNELS * MAX_SAMPLES_PER_CHANNEL];
int64 metric_sample_numbers[MAX_SAMPLES_PER_CHANNEL]; 
uint64 metric_event_codes[MAX_SAMPLES_PER_CHANNEL];
double metric_timestamps[MAX_SAMPLES_PER_CHANNEL];



DataBuffer* metricsDataBuffer;

// UDP variables
int port = 8080;
int data_channels = 5;
int gui_refresh_min = 300;
float data_scale = 25;

std::atomic<int> packet_queue_count(0);
std::atomic<float> udp_values[MAX_SAMPLES_PER_CHANNEL * MAX_DATA_CHANNELS];
std::atomic<int> server_running(0);
std::atomic<int> server_closed(0);
std::atomic<int> point_per_packet(1);

typedef std::chrono::high_resolution_clock::time_point tp;
tp get_time()
{
	return std::chrono::high_resolution_clock::now();
}


static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int udp_thread_function() {
    LOGD("Attempting to listen on port ", port);
    // Create UDP socket (IPv4)
	
	if (port == -1)
		return 1;
	
	std::array<char, 65536> buf{}; // max UDP payload size (practical)

	constexpr int MAX_EVENTS = 64;
	std::array<epoll_event, MAX_EVENTS> events;

	// UDP stuff idk
	int sock; // UDP socket
	int sfd;
	int ep;
	
    sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == -1) {
        LOGD("socket");
        return 1;
    }

    if (set_nonblocking(sock) == -1) {
        LOGD("fcntl(O_NONBLOCK)");
        return 1;
    }

	int yes = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)); // optional, Linux-specific

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


    LOGD("UDP server listening on port ", port);
	server_running = true;

	

	while (server_running) {
		int n = epoll_wait(ep, events.data(), MAX_EVENTS, -1);
		if (n == -1) {
			if (errno == EINTR) continue;
			LOGD("epoll_wait");
			continue;
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
				while (server_running) {
					

					sockaddr_in src{};
					socklen_t srclen = sizeof(src);

					ssize_t r = recvfrom(sock, buf.data(), buf.size(), 0,
										 reinterpret_cast<sockaddr*>(&src), &srclen);
					if (r > 0) {
						// Example "processing": print and echo back
						char ip[INET_ADDRSTRLEN];
						inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
						uint16_t sport = ntohs(src.sin_port);

						short *data = (short*) buf.data();

						if (MAX_SAMPLES_PER_CHANNEL <= packet_queue_count)
						{
							LOGD("Forced to drop packet");
							break;
						}

						for (int j = 0; j < data_channels; j++)
						{
							udp_values[j*MAX_SAMPLES_PER_CHANNEL + packet_queue_count] = data[j];
						}

						//LOGD("1: ", data[3], ", 2: ", data[4]);

						packet_queue_count++;
						// LOGD("Data", *((float*) buf.data()));

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


		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}


	// Closing the socket
	if (sock != -1) {
        epoll_ctl(ep, EPOLL_CTL_DEL, sock, nullptr); // remove from epoll
        close(sock);
        sock = -1;
        std::cout << "Closed UDP socket\n";
    }

    if (sfd != -1) {
        epoll_ctl(ep, EPOLL_CTL_DEL, sfd, nullptr); // remove signal fd
        close(sfd);
        sfd = -1;
        std::cout << "Closed signal fd\n";
    }

    if (ep != -1) {
        close(ep);
        ep = -1;
        std::cout << "Closed epoll instance\n";
    }

	server_closed = true;
	LOGD("Closed Plugin");

	return 0;
    
}

void close_udp_thread()
{
	// Check if existing server is running
	if (server_running)
	{
		// Close existing thread
		server_closed = false;
		server_running = false; // Actually closes thread

		LOGD("Attempt to close server");

		// Wait till server has closed
		while (!server_closed)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

void restart_thread()
{
	close_udp_thread();	

	server_closed = false;
	std::thread t(udp_thread_function);
	t.detach();
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

	LOGD("Update Settings");

	DataStream::Settings packet_stream_settings
	{
	   "UDP Packet Stream", // stream name
	   "Pulls data from UDP packets",   // stream description
	   "identifier",    // stream identifier
	   30000.0          // stream sample rate
	};

	DataStream::Settings packet_rate_stream_settings
	{
	   "UDP Packet Rate", // stream name
	   "Rate at which packets are being transfered",   // stream description
	   "identifier",    // stream identifier
	   30000.0          // stream sample rate
	};

	DataStream* packet_stream = new DataStream(packet_stream_settings);
	DataStream* packet_rate_stream = new DataStream(packet_rate_stream_settings);

	sourceStreams->add(packet_stream); // add pointer to owned array
	sourceStreams->add(packet_rate_stream); // add pointer to owned array

	// create a data buffer and add it to the sourceBuffer array
	sourceBuffers.add(new DataBuffer(MAX_DATA_CHANNELS, 48000));
	dataBuffer = sourceBuffers.getLast();

	sourceBuffers.add(new DataBuffer(METRICS_CHANNELS, 48000));
	metricsDataBuffer = sourceBuffers.getLast();

	// packet channels
	for (int i = 0; i < MAX_DATA_CHANNELS; i++)
	{
	   ContinuousChannel::Settings settings{
	                          ContinuousChannel::Type::ELECTRODE, // channel type
	                          "CH" + String(i+1), // channel name
	                          "description",      // channel description
	                          "identifier",       // channel identifier
	                          0.195,              // channel bitvolts scaling
	                          packet_stream              // associated data stream
	                  };

	   continuousChannels->add(new ContinuousChannel(settings));
	}

	// setting channels
	ContinuousChannel::Settings settings{
						  ContinuousChannel::Type::ELECTRODE, // channel type
						  "Packet Rate", // channel name
						  "description",      // channel description
						  "identifier",       // channel identifier
						  0.195,              // channel bitvolts scaling
						  packet_rate_stream              // associated data stream
				  };

	continuousChannels->add(new ContinuousChannel(settings));

	// Not sure if this is needed
	EventChannel::Settings settings2{
	                  EventChannel::Type::TTL, // channel type (must be TTL)
	                  "Device Event Channel",  // channel name
	                  "description",           // channel description
	                  "identifier",            // channel identifier
	                  packet_stream,                  // associated data stream
	                  8                        // maximum number of TTL lines
	          };

	eventChannels->add(new EventChannel(settings2));
}

bool DataThreadPlugin::startAcquisition()
{
	startThread();

	for (int i = 0; i < MAX_SAMPLES_PER_CHANNEL * MAX_DATA_CHANNELS; i++)
		udp_values[i] = 0;

	restart_thread(); // Start UDP thread
	return true;
}


tp last_buffer_update;
float packet_rate = 0;
bool DataThreadPlugin::updateBuffer()
{

	int packet_count = packet_queue_count; // prevent multithreading weirdness
	if (packet_count < gui_refresh_min)
	{

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		return true;
	}

	for (int i = 0; i < packet_count; i++)
	{
		for (int j = 0; j < data_channels; j++)
		{
			data_points[j * packet_count + i] = udp_values[j * MAX_SAMPLES_PER_CHANNEL + i] * data_scale;
		}


		

		sample_numbers[i] = totalSamples++;
	}

	// Filling other channels with 0s
	for (int i = data_channels*packet_count; i < MAX_DATA_CHANNELS*packet_count; i++)
	{

		data_points[i] = 0;
	}

	packet_queue_count = 0;

	dataBuffer->addToBuffer(data_points,
                           sample_numbers,
                           timestamps,
                           event_codes,
                           packet_count);

	// Metrics

	tp t = get_time();
	long deltatime = std::chrono::duration_cast<std::chrono::microseconds>(t - last_buffer_update).count() + 1; // +1 to prevent  / 0 errors

	const float to_seconds = 0.000001;

	float packet_avg = 0.1;

	float rate = (float) packet_count / (deltatime * to_seconds);
	packet_rate = packet_rate * (1 - packet_avg) + rate * packet_avg;

	last_buffer_update = t;


	metric_data_points[0] = packet_rate;
	metric_sample_numbers[0] = totalSamples++;

	metricsDataBuffer->addToBuffer(metric_data_points, 
								   metric_sample_numbers, 
								   metric_timestamps, 
								   metric_event_codes, 
								   1);
	

	return true;

}

bool DataThreadPlugin::stopAcquisition()
{
	last_buffer_update = get_time();
	if (isThreadRunning())
	{
	  signalThreadShouldExit(); //stop thread
	}

	close_udp_thread();

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
		int new_port = param->getValue();	
		if (server_running)
			restart_thread();
		else
			port = new_port;

		LOGD ("Port changed to ", port); // log message
	
   }
	else if (param->getName().equalsIgnoreCase ("scale"))
   {
	   data_scale = param->getValue();
   }
	else if (param->getName().equalsIgnoreCase ("channels"))
   {
	   data_channels = param->getValue();
   }
	else if (param->getName().equalsIgnoreCase ("packet_hold"))
   {
	   gui_refresh_min = param->getValue();
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
	addIntParameter (Parameter::PROCESSOR_SCOPE, // parameter scope
                     "channels", // parameter name
                     "Channels", // display name
                     "Number of channels to pull data from, arbitrary max", // parameter description
                     1, // default value
                     0, // minimum value
                     MAX_DATA_CHANNELS, // maximum value
                     false); 

	addIntParameter (Parameter::PROCESSOR_SCOPE, // parameter scope
                     "packet_hold", // parameter name
                     "Packet Hold", // display name
                     "Number of packets before plugin will write to buffer (improves performace probably)", // parameter description
                     300, // default value
                     0, // minimum value
                     1000, // maximum value
                     false); 



	addFloatParameter (Parameter::PROCESSOR_SCOPE, // parameter scope
                     "scale", // parameter name
                     "Data Scale", // display name
                     "Scale of all channels", // parameter description
					 "Unit",
                     25, // default value
                     0, // minimum value
                     15000, // maximum value
					 0.25,
                     false); 
}


