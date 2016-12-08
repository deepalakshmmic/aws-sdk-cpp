/*
* Copyright 2010-2016 Amazon.com, Inc. or its affiliates. All Rights Reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License").
* You may not use this file except in compliance with the License.
* A copy of the License is located at
*
*  http://aws.amazon.com/apache2.0
*
* or in the "license" file accompanying this file. This file is distributed
* on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
* express or implied. See the License for the specific language governing
* permissions and limitations under the License.
*/

#include <aws/polly-to-output/TextToSpeechManager.h>
#include <aws/polly/model/SynthesizeSpeechRequest.h>
#include <aws/polly/model/DescribeVoicesRequest.h>
#include <aws/core/utils/Outcome.h>

using namespace Aws::Polly;
using namespace Aws::Polly::Model;
using namespace Aws::Utils;

namespace Aws
{
    namespace PollyToOutput
    {
        static const size_t BUFF_SIZE = 8192;

        TextToSpeechManager::TextToSpeechManager(const std::shared_ptr<Polly::PollyClient>& pollyClient, const std::shared_ptr<PCMOutputDriverFactory>& driverFactory) 
            : m_pollyClient(pollyClient), m_activeVoice(VoiceId::Kimberly)
        {
            m_drivers = (driverFactory ? driverFactory : DefaultPCMOutputDriverFactoryInitFn())->LoadDrivers();
        }

        TextToSpeechManager::~TextToSpeechManager()
        {
        }

        void TextToSpeechManager::SendTextToOutputDevice(const char* text, SendTextCompletedHandler)
        {
            if (!m_activeDriver)
            {
                auto&& devices = EnumerateDevices();
                assert(devices.size() > 0);

                SetActiveDevice(devices.front().second, devices.front().first, devices.front().first.capabilities.front());
            }

            SynthesizeSpeechRequest synthesizeSpeechRequest;
            synthesizeSpeechRequest.WithOutputFormat(OutputFormat::pcm)
                .WithSampleRate(StringUtils::to_string(m_selectedCaps.sampleRate))
                .WithTextType(TextType::text)
                .WithText(text)
                .WithVoiceId(m_activeVoice);

            m_pollyClient->SynthesizeSpeechAsync(synthesizeSpeechRequest, [this](const Polly::PollyClient* client, const Polly::Model::SynthesizeSpeechRequest& request,
                const Polly::Model::SynthesizeSpeechOutcome& speechOutcome, const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context)
            {OnPollySynthSpeechOutcomeRecieved(client, request, speechOutcome, context);});
        }

        Aws::Vector<std::pair<DeviceInfo, std::shared_ptr<PCMOutputDriver>>> TextToSpeechManager::EnumerateDevices() const
        {
            Aws::Vector<std::pair<DeviceInfo, std::shared_ptr<PCMOutputDriver>>> deviceDriverList;

            for (auto& driver : m_drivers)
            {
                for (auto& deviceInfo : driver->EnumerateDevices())
                {
                    std::pair<DeviceInfo, std::shared_ptr<PCMOutputDriver>> device(deviceInfo, driver);
                    deviceDriverList.push_back(device);
                }
            }
            
            return deviceDriverList;
        }

        void TextToSpeechManager::SetActiveDevice(const std::shared_ptr<PCMOutputDriver>& driver, const DeviceInfo& device, const CapabilityInfo& caps)
        {
            std::lock_guard<std::mutex> m(m_driverLock);
            driver->SetActiveDevice(device, caps);
            m_activeDriver = driver;
            m_selectedCaps = caps;
        }

        Aws::Vector<std::pair<Aws::String, Aws::String>> TextToSpeechManager::ListAvailableVoices() const
        {
            Aws::Vector<std::pair<Aws::String, Aws::String>> m_voices;

            DescribeVoicesRequest describeVoices;
            
            auto voicesOutcome = m_pollyClient->DescribeVoices(describeVoices);
            if (voicesOutcome.IsSuccess())
            {
                for (auto& voice : voicesOutcome.GetResult().GetVoices())
                {
                    m_voices.push_back(std::pair<Aws::String, Aws::String>(voice.GetName(), voice.GetLanguageName()));
                }
            }

            return m_voices;
        }

        void TextToSpeechManager::SetActiveVoice(const Aws::String& voice)
        {
            m_activeVoice = VoiceIdMapper::GetVoiceIdForName(voice);
        }

        void TextToSpeechManager::OnPollySynthSpeechOutcomeRecieved(const Polly::PollyClient*, const Polly::Model::SynthesizeSpeechRequest&,
            const Polly::Model::SynthesizeSpeechOutcome& outcome, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) const
        {
            if(outcome.IsSuccess())
            {
                auto result = const_cast<Polly::Model::SynthesizeSpeechOutcome&>(outcome).GetResultWithOwnership();
                auto& stream = result.GetAudioStream();

                std::streamsize amountRead(0);
                unsigned char buffer[BUFF_SIZE];

                std::lock_guard<std::mutex> m(m_driverLock);
                m_activeDriver->Prime();

                while (stream)
                {
                    stream.read((char*) buffer, BUFF_SIZE);
                    auto read = stream.gcount();
                    m_activeDriver->WriteBufferToDevice(buffer, read);
                    amountRead += read;
                }

                m_activeDriver->Flush();
            }
        }
    }
}
