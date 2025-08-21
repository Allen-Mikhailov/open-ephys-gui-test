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

void DataThreadPlugin::registerParameters()
{
    // Register parameters for the plugin here (e.g. addParameter())
}

void DataThreadPlugin::parameterValueChanged (Parameter* parameter)
{
    // Handle parameter value changes here (e.g. update settings)
}
