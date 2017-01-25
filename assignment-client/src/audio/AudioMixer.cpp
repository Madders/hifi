//
//  AudioMixer.cpp
//  assignment-client/src/audio
//
//  Created by Stephen Birarda on 8/22/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <thread>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

#include <LogHandler.h>
#include <NetworkAccessManager.h>
#include <NodeList.h>
#include <Node.h>
#include <OctreeConstants.h>
#include <plugins/PluginManager.h>
#include <plugins/CodecPlugin.h>
#include <udt/PacketHeaders.h>
#include <SharedUtil.h>
#include <StDev.h>
#include <UUID.h>

#include "AudioHelpers.h"
#include "AudioRingBuffer.h"
#include "AudioMixerClientData.h"
#include "AvatarAudioStream.h"
#include "InjectedAudioStream.h"

#include "AudioMixer.h"

static const float LOUDNESS_TO_DISTANCE_RATIO = 0.00001f;
static const float DEFAULT_ATTENUATION_PER_DOUBLING_IN_DISTANCE = 0.5f;    // attenuation = -6dB * log2(distance)
static const float DEFAULT_NOISE_MUTING_THRESHOLD = 0.003f;
static const QString AUDIO_MIXER_LOGGING_TARGET_NAME = "audio-mixer";
static const QString AUDIO_ENV_GROUP_KEY = "audio_env";
static const QString AUDIO_BUFFER_GROUP_KEY = "audio_buffer";
static const QString AUDIO_THREADING_GROUP_KEY = "audio_threading";

int AudioMixer::_numStaticJitterFrames{ -1 };
float AudioMixer::_noiseMutingThreshold{ DEFAULT_NOISE_MUTING_THRESHOLD };
float AudioMixer::_attenuationPerDoublingInDistance{ DEFAULT_ATTENUATION_PER_DOUBLING_IN_DISTANCE };
QHash<QString, AABox> AudioMixer::_audioZones;
QVector<AudioMixer::ZoneSettings> AudioMixer::_zoneSettings;
QVector<AudioMixer::ReverbSettings> AudioMixer::_zoneReverbSettings;

AudioMixer::AudioMixer(ReceivedMessage& message) :
    ThreadedAssignment(message) {
    auto nodeList = DependencyManager::get<NodeList>();
    auto& packetReceiver = nodeList->getPacketReceiver();

    packetReceiver.registerListenerForTypes({ PacketType::MicrophoneAudioNoEcho, PacketType::MicrophoneAudioWithEcho,
                                              PacketType::InjectAudio, PacketType::SilentAudioFrame,
                                              PacketType::AudioStreamStats },
                                            this, "handleNodeAudioPacket");
    packetReceiver.registerListener(PacketType::NegotiateAudioFormat, this, "handleNegotiateAudioFormat");
    packetReceiver.registerListener(PacketType::MuteEnvironment, this, "handleMuteEnvironmentPacket");
    packetReceiver.registerListener(PacketType::NodeIgnoreRequest, this, "handleNodeIgnoreRequestPacket");
    packetReceiver.registerListener(PacketType::KillAvatar, this, "handleKillAvatarPacket");
    packetReceiver.registerListener(PacketType::NodeMuteRequest, this, "handleNodeMuteRequestPacket");
    packetReceiver.registerListener(PacketType::RadiusIgnoreRequest, this, "handleRadiusIgnoreRequestPacket");
    packetReceiver.registerListener(PacketType::RequestsDomainListData, this, "handleRequestsDomainListDataPacket"); 
    packetReceiver.registerListener(PacketType::PerAvatarGainSet, this, "handlePerAvatarGainSetDataPacket");

    connect(nodeList.data(), &NodeList::nodeKilled, this, &AudioMixer::handleNodeKilled);
}

void AudioMixer::handleNodeAudioPacket(QSharedPointer<ReceivedMessage> message, SharedNodePointer sendingNode) {
    getOrCreateClientData(sendingNode.data());
    DependencyManager::get<NodeList>()->updateNodeWithDataFromPacket(message, sendingNode);
}

void AudioMixer::handleMuteEnvironmentPacket(QSharedPointer<ReceivedMessage> message, SharedNodePointer sendingNode) {
    auto nodeList = DependencyManager::get<NodeList>();

    if (sendingNode->getCanKick()) {
        glm::vec3 position;
        float radius;

        auto newPacket = NLPacket::create(PacketType::MuteEnvironment, sizeof(position) + sizeof(radius));

        // read the position and radius from the sent packet
        message->readPrimitive(&position);
        message->readPrimitive(&radius);

        // write them to our packet
        newPacket->writePrimitive(position);
        newPacket->writePrimitive(radius);

        nodeList->eachNode([&](const SharedNodePointer& node){
            if (node->getType() == NodeType::Agent && node->getActiveSocket() &&
                node->getLinkedData() && node != sendingNode) {
                nodeList->sendUnreliablePacket(*newPacket, *node);
            }
        });
    }
}

DisplayPluginList getDisplayPlugins() {
    DisplayPluginList result;
    return result;
}

InputPluginList getInputPlugins() {
    InputPluginList result;
    return result;
}

void saveInputPluginSettings(const InputPluginList& plugins) {
}


void AudioMixer::handleNegotiateAudioFormat(QSharedPointer<ReceivedMessage> message, SharedNodePointer sendingNode) {
    QStringList availableCodecs;
    auto codecPlugins = PluginManager::getInstance()->getCodecPlugins();
    if (codecPlugins.size() > 0) {
        for (auto& plugin : codecPlugins) {
            auto codecName = plugin->getName();
            qDebug() << "Codec available:" << codecName;
            availableCodecs.append(codecName);
        }
    } else {
        qDebug() << "No Codecs available...";
    }

    CodecPluginPointer selectedCodec;
    QString selectedCodecName;

    QStringList codecPreferenceList = _codecPreferenceOrder.split(",");

    // read the codecs requested by the client
    const int MAX_PREFERENCE = 99999;
    int preferredCodecIndex = MAX_PREFERENCE;
    QString preferredCodec;
    quint8 numberOfCodecs = 0;
    message->readPrimitive(&numberOfCodecs);
    qDebug() << "numberOfCodecs:" << numberOfCodecs;
    QStringList codecList;
    for (quint16 i = 0; i < numberOfCodecs; i++) {
        QString requestedCodec = message->readString();
        int preferenceOfThisCodec = codecPreferenceList.indexOf(requestedCodec);
        bool codecAvailable = availableCodecs.contains(requestedCodec);
        qDebug() << "requestedCodec:" << requestedCodec << "preference:" << preferenceOfThisCodec << "available:" << codecAvailable;
        if (codecAvailable) {
            codecList.append(requestedCodec);
            if (preferenceOfThisCodec >= 0 && preferenceOfThisCodec < preferredCodecIndex) {
                qDebug() << "This codec is preferred...";
                selectedCodecName  = requestedCodec;
                preferredCodecIndex = preferenceOfThisCodec;
            }
        }
    }
    qDebug() << "all requested and available codecs:" << codecList;

    // choose first codec
    if (!selectedCodecName.isEmpty()) {
        if (codecPlugins.size() > 0) {
            for (auto& plugin : codecPlugins) {
                if (selectedCodecName == plugin->getName()) {
                    qDebug() << "Selecting codec:" << selectedCodecName;
                    selectedCodec = plugin;
                    break;
                }
            }
        }
    }

    auto clientData = getOrCreateClientData(sendingNode.data());
    clientData->setupCodec(selectedCodec, selectedCodecName);
    qDebug() << "selectedCodecName:" << selectedCodecName;
    clientData->sendSelectAudioFormat(sendingNode, selectedCodecName);
}

void AudioMixer::handleNodeKilled(SharedNodePointer killedNode) {
    // enumerate the connected listeners to remove HRTF objects for the disconnected node
    auto nodeList = DependencyManager::get<NodeList>();

    nodeList->eachNode([&killedNode](const SharedNodePointer& node) {
        auto clientData = dynamic_cast<AudioMixerClientData*>(node->getLinkedData());
        if (clientData) {
            QUuid killedUUID = killedNode->getUUID();
            clientData->removeHRTFsForNode(killedUUID);
        }
    });
}

void AudioMixer::handleNodeMuteRequestPacket(QSharedPointer<ReceivedMessage> packet, SharedNodePointer sendingNode) {
    auto nodeList = DependencyManager::get<NodeList>();
    QUuid nodeUUID = QUuid::fromRfc4122(packet->readWithoutCopy(NUM_BYTES_RFC4122_UUID));
    if (sendingNode->getCanKick()) {
        auto node = nodeList->nodeWithUUID(nodeUUID);
        if (node) {
            // we need to set a flag so we send them the appropriate packet to mute them
            AudioMixerClientData* nodeData = (AudioMixerClientData*)node->getLinkedData();
            nodeData->setShouldMuteClient(true);
        } else {
            qWarning() << "Node mute packet received for unknown node " << uuidStringWithoutCurlyBraces(nodeUUID);
        }
    } else {
        qWarning() << "Node mute packet received from node that cannot mute, ignoring";
    }
}

void AudioMixer::handleKillAvatarPacket(QSharedPointer<ReceivedMessage> packet, SharedNodePointer sendingNode) {
    auto clientData = dynamic_cast<AudioMixerClientData*>(sendingNode->getLinkedData());
    if (clientData) {
        clientData->removeAgentAvatarAudioStream();
        auto nodeList = DependencyManager::get<NodeList>();
        nodeList->eachNode([sendingNode](const SharedNodePointer& node){
            auto listenerClientData = dynamic_cast<AudioMixerClientData*>(node->getLinkedData());
            if (listenerClientData) {
                listenerClientData->removeHRTFForStream(sendingNode->getUUID());
            }
        });
    }
}

void AudioMixer::handleRequestsDomainListDataPacket(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    auto nodeList = DependencyManager::get<NodeList>();
    nodeList->getOrCreateLinkedData(senderNode);

    if (senderNode->getLinkedData()) {
        AudioMixerClientData* nodeData = dynamic_cast<AudioMixerClientData*>(senderNode->getLinkedData());
        if (nodeData != nullptr) {
            bool isRequesting;
            message->readPrimitive(&isRequesting);
            nodeData->setRequestsDomainListData(isRequesting);
        }
    }
}

void AudioMixer::handleNodeIgnoreRequestPacket(QSharedPointer<ReceivedMessage> packet, SharedNodePointer sendingNode) {
    sendingNode->parseIgnoreRequestMessage(packet);
}

void AudioMixer::handlePerAvatarGainSetDataPacket(QSharedPointer<ReceivedMessage> packet, SharedNodePointer sendingNode) {
    auto clientData = dynamic_cast<AudioMixerClientData*>(sendingNode->getLinkedData());
    if (clientData) {
        QUuid listeningNodeUUID = sendingNode->getUUID();
        // parse the UUID from the packet
        QUuid audioSourceUUID = QUuid::fromRfc4122(packet->readWithoutCopy(NUM_BYTES_RFC4122_UUID));
        uint8_t packedGain;
        packet->readPrimitive(&packedGain);
        float gain = unpackFloatGainFromByte(packedGain);
        clientData->hrtfForStream(audioSourceUUID, QUuid()).setGainAdjustment(gain);
        qDebug() << "Setting gain adjustment for hrtf[" << listeningNodeUUID << "][" << audioSourceUUID << "] to " << gain;
    }
}

void AudioMixer::handleRadiusIgnoreRequestPacket(QSharedPointer<ReceivedMessage> packet, SharedNodePointer sendingNode) {
    sendingNode->parseIgnoreRadiusRequestMessage(packet);
}

void AudioMixer::removeHRTFsForFinishedInjector(const QUuid& streamID) {
    auto injectorClientData = qobject_cast<AudioMixerClientData*>(sender());
    if (injectorClientData) {
        // enumerate the connected listeners to remove HRTF objects for the disconnected injector
        auto nodeList = DependencyManager::get<NodeList>();

        nodeList->eachNode([injectorClientData, &streamID](const SharedNodePointer& node){
            auto listenerClientData = dynamic_cast<AudioMixerClientData*>(node->getLinkedData());
            if (listenerClientData) {
                listenerClientData->removeHRTFForStream(injectorClientData->getNodeID(), streamID);
            }
        });
    }
}

QString AudioMixer::percentageForMixStats(int counter) {
    if (_stats.totalMixes > 0) {
        float mixPercentage = (float(counter) / _stats.totalMixes) * 100.0f;
        return QString::number(mixPercentage, 'f', 2);
    } else {
        return QString("0.0");
    }
}

void AudioMixer::sendStatsPacket() {
    QJsonObject statsObject;

    if (_numStatFrames == 0) {
        return;
    }

    // general stats
    statsObject["useDynamicJitterBuffers"] = _numStaticJitterFrames == -1;

    statsObject["threads"] = _slavePool.numThreads();

    statsObject["trailing_mix_ratio"] = _trailingMixRatio;
    statsObject["throttling_ratio"] = _throttlingRatio;

    statsObject["avg_streams_per_frame"] = (float)_stats.sumStreams / (float)_numStatFrames;
    statsObject["avg_listeners_per_frame"] = (float)_stats.sumListeners / (float)_numStatFrames;

    // timing stats
    QJsonObject timingStats;

    auto addTiming = [&](Timer& timer, std::string name) {
        uint64_t timing, trailing;
        timer.get(timing, trailing);
        timingStats[("us_per_" + name).c_str()] = (qint64)(timing / _numStatFrames);
        timingStats[("us_per_" + name + "_trailing").c_str()] = (qint64)(trailing / _numStatFrames);
    };

    addTiming(_ticTiming, "tic");
    addTiming(_sleepTiming, "sleep");
    addTiming(_frameTiming, "frame");
    addTiming(_prepareTiming, "prepare");
    addTiming(_mixTiming, "mix");
    addTiming(_eventsTiming, "events");

    // call it "avg_..." to keep it higher in the display, sorted alphabetically
    statsObject["avg_timing_stats"] = timingStats;

    // mix stats
    QJsonObject mixStats;

    mixStats["%_hrtf_mixes"] = percentageForMixStats(_stats.hrtfRenders);
    mixStats["%_hrtf_silent_mixes"] = percentageForMixStats(_stats.hrtfSilentRenders);
    mixStats["%_hrtf_throttle_mixes"] = percentageForMixStats(_stats.hrtfThrottleRenders);
    mixStats["%_manual_stereo_mixes"] = percentageForMixStats(_stats.manualStereoMixes);
    mixStats["%_manual_echo_mixes"] = percentageForMixStats(_stats.manualEchoMixes);

    mixStats["total_mixes"] = _stats.totalMixes;
    mixStats["avg_mixes_per_block"] = _stats.totalMixes / _numStatFrames;

    statsObject["mix_stats"] = mixStats;

    _numStatFrames = 0;
    _stats.reset();

    // add stats for each listerner
    auto nodeList = DependencyManager::get<NodeList>();
    QJsonObject listenerStats;

    nodeList->eachNode([&](const SharedNodePointer& node) {
        AudioMixerClientData* clientData = static_cast<AudioMixerClientData*>(node->getLinkedData());
        if (clientData) {
            QJsonObject nodeStats;
            QString uuidString = uuidStringWithoutCurlyBraces(node->getUUID());

            nodeStats["outbound_kbps"] = node->getOutboundBandwidth();
            nodeStats[USERNAME_UUID_REPLACEMENT_STATS_KEY] = uuidString;

            nodeStats["jitter"] = clientData->getAudioStreamStats();

            listenerStats[uuidString] = nodeStats;
        }
    });

    // add the listeners object to the root object
    statsObject["z_listeners"] = listenerStats;

    // send off the stats packets
    ThreadedAssignment::addPacketStatsAndSendStatsPacket(statsObject);
}

void AudioMixer::run() {

    qDebug() << "Waiting for connection to domain to request settings from domain-server.";

    // wait until we have the domain-server settings, otherwise we bail
    DomainHandler& domainHandler = DependencyManager::get<NodeList>()->getDomainHandler();
    connect(&domainHandler, &DomainHandler::settingsReceived, this, &AudioMixer::start);
    connect(&domainHandler, &DomainHandler::settingsReceiveFail, this, &AudioMixer::domainSettingsRequestFailed);

    ThreadedAssignment::commonInit(AUDIO_MIXER_LOGGING_TARGET_NAME, NodeType::AudioMixer);
}

AudioMixerClientData* AudioMixer::getOrCreateClientData(Node* node) {
    auto clientData = dynamic_cast<AudioMixerClientData*>(node->getLinkedData());

    if (!clientData) {
        node->setLinkedData(std::unique_ptr<NodeData> { new AudioMixerClientData(node->getUUID()) });
        clientData = dynamic_cast<AudioMixerClientData*>(node->getLinkedData());
        connect(clientData, &AudioMixerClientData::injectorStreamFinished, this, &AudioMixer::removeHRTFsForFinishedInjector);
    }

    return clientData;
}

void AudioMixer::start() {
    auto nodeList = DependencyManager::get<NodeList>();

    // prepare the NodeList
    nodeList->addNodeTypeToInterestSet(NodeType::Agent);
    nodeList->linkedDataCreateCallback = [&](Node* node) { getOrCreateClientData(node); };

    // parse out any AudioMixer settings
    {
        DomainHandler& domainHandler = nodeList->getDomainHandler();
        const QJsonObject& settingsObject = domainHandler.getSettingsObject();
        parseSettingsObject(settingsObject);
    }

    // mix state
    unsigned int frame = 1;
    auto frameTimestamp = p_high_resolution_clock::now();

    while (!_isFinished) {
        auto ticTimer = _ticTiming.timer();

        {
            auto timer = _sleepTiming.timer();
            auto frameDuration = timeFrame(frameTimestamp);
            throttle(frameDuration, frame);
        }

        auto frameTimer = _frameTiming.timer();

        nodeList->nestedEach([&](NodeList::const_iterator cbegin, NodeList::const_iterator cend) {
            // prepare frames; pop off any new audio from their streams
            {
                auto prepareTimer = _prepareTiming.timer();
                std::for_each(cbegin, cend, [&](const SharedNodePointer& node) {
                    _stats.sumStreams += prepareFrame(node, frame);
                });
            }

            // mix across slave threads
            {
                auto mixTimer = _mixTiming.timer();
                _slavePool.mix(cbegin, cend, frame, _throttlingRatio);
            }
        });

        // gather stats
        _slavePool.each([&](AudioMixerSlave& slave) {
            _stats.accumulate(slave.stats);
            slave.stats.reset();
        });

        ++frame;
        ++_numStatFrames;

        // play nice with qt event-looping
        {
            auto eventsTimer = _eventsTiming.timer();

            // since we're a while loop we need to yield to qt's event processing
            QCoreApplication::processEvents();

            if (_isFinished) {
                // alert qt eventing that this is finished
                QCoreApplication::sendPostedEvents(this, QEvent::DeferredDelete);
                break;
            }
        }
    }
}

std::chrono::microseconds AudioMixer::timeFrame(p_high_resolution_clock::time_point& timestamp) {
    // advance the next frame
    auto nextTimestamp = timestamp + std::chrono::microseconds(AudioConstants::NETWORK_FRAME_USECS);
    auto now = p_high_resolution_clock::now();

    // compute how long the last frame took
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - timestamp);

    // set the new frame timestamp
    timestamp = std::max(now, nextTimestamp);

    // sleep until the next frame should start
    std::this_thread::sleep_until(timestamp);

    return duration;
}

void AudioMixer::throttle(std::chrono::microseconds duration, int frame) {
    // throttle using a modified integral controller
    const float FRAME_TIME = 10000.0f;
    float mixRatio = duration.count() / FRAME_TIME;

    // constants are determined based on a "regular" 16-CPU EC2 server

    // target different mix and backoff ratios (they also have different backoff rates)
    // this is to prevent oscillation, and encourage throttling to find a steady state
    const float TARGET = 0.9f;
    // on a "regular" machine with 100 avatars, this is the largest value where
    // - overthrottling can be recovered
    // - oscillations will not occur after the recovery
    const float BACKOFF_TARGET = 0.44f;

    // the mixer is known to struggle at about 80 on a "regular" machine
    // so throttle 2/80 the streams to ensure smooth audio (throttling is linear)
    const float THROTTLE_RATE = 2 / 80.0f;
    const float BACKOFF_RATE = THROTTLE_RATE / 4;

    // recovery should be bounded so that large changes in user count is a tolerable experience
    // throttling is linear, so most cases will not need a full recovery
    const int RECOVERY_TIME = 180;

    // weight more recent frames to determine if throttling is necessary,
    const int TRAILING_FRAMES = (int)(100 * RECOVERY_TIME * BACKOFF_RATE);
    const float CURRENT_FRAME_RATIO = 1.0f / TRAILING_FRAMES;
    const float PREVIOUS_FRAMES_RATIO = 1.0f - CURRENT_FRAME_RATIO;
    _trailingMixRatio = PREVIOUS_FRAMES_RATIO * _trailingMixRatio + CURRENT_FRAME_RATIO * mixRatio;

    if (frame % TRAILING_FRAMES == 0) {
        if (_trailingMixRatio > TARGET) {
            _throttlingRatio += THROTTLE_RATE;
            _throttlingRatio = std::min(_throttlingRatio, 1.0f);
            qDebug("audio-mixer is struggling (%f mix/sleep) - throttling %f of streams",
                    (double)_trailingMixRatio, (double)_throttlingRatio);
        } else if (_throttlingRatio > 0.0f && _trailingMixRatio <= BACKOFF_TARGET) {
            _throttlingRatio -= BACKOFF_RATE;
            _throttlingRatio = std::max(_throttlingRatio, 0.0f);
            qDebug("audio-mixer is recovering (%f mix/sleep) - throttling %f of streams",
                    (double)_trailingMixRatio, (double)_throttlingRatio);
        }
    }
}

int AudioMixer::prepareFrame(const SharedNodePointer& node, unsigned int frame) {
    AudioMixerClientData* data = (AudioMixerClientData*)node->getLinkedData();
    if (data == nullptr) {
        return 0;
    }

    return data->checkBuffersBeforeFrameSend();
}

void AudioMixer::parseSettingsObject(const QJsonObject &settingsObject) {
    if (settingsObject.contains(AUDIO_THREADING_GROUP_KEY)) {
        QJsonObject audioThreadingGroupObject = settingsObject[AUDIO_THREADING_GROUP_KEY].toObject();
        const QString AUTO_THREADS = "auto_threads";
        bool autoThreads = audioThreadingGroupObject[AUTO_THREADS].toBool();
        if (!autoThreads) {
            bool ok;
            const QString NUM_THREADS = "num_threads";
            int numThreads = audioThreadingGroupObject[NUM_THREADS].toString().toInt(&ok);
            if (ok) {
                _slavePool.setNumThreads(numThreads);
            }
        }
    }

    if (settingsObject.contains(AUDIO_BUFFER_GROUP_KEY)) {
        QJsonObject audioBufferGroupObject = settingsObject[AUDIO_BUFFER_GROUP_KEY].toObject();

        // check the payload to see if we have asked for dynamicJitterBuffer support
        const QString DYNAMIC_JITTER_BUFFER_JSON_KEY = "dynamic_jitter_buffer";
        bool enableDynamicJitterBuffer = audioBufferGroupObject[DYNAMIC_JITTER_BUFFER_JSON_KEY].toBool();
        if (enableDynamicJitterBuffer) {
            qDebug() << "Enabling dynamic jitter buffers.";

            bool ok;
            const QString DESIRED_JITTER_BUFFER_FRAMES_KEY = "static_desired_jitter_buffer_frames";
            _numStaticJitterFrames = audioBufferGroupObject[DESIRED_JITTER_BUFFER_FRAMES_KEY].toString().toInt(&ok);
            if (!ok) {
                _numStaticJitterFrames = InboundAudioStream::DEFAULT_STATIC_JITTER_FRAMES;
            }
            qDebug() << "Static desired jitter buffer frames:" << _numStaticJitterFrames;
        } else {
            qDebug() << "Disabling dynamic jitter buffers.";
            _numStaticJitterFrames = -1;
        }

        // check for deprecated audio settings
        auto deprecationNotice = [](const QString& setting, const QString& value) {
            qInfo().nospace() << "[DEPRECATION NOTICE] " << setting << "(" << value << ") has been deprecated, and has no effect";
        };
        bool ok;

        const QString MAX_FRAMES_OVER_DESIRED_JSON_KEY = "max_frames_over_desired";
        int maxFramesOverDesired = audioBufferGroupObject[MAX_FRAMES_OVER_DESIRED_JSON_KEY].toString().toInt(&ok);
        if (ok && maxFramesOverDesired != InboundAudioStream::MAX_FRAMES_OVER_DESIRED) {
            deprecationNotice(MAX_FRAMES_OVER_DESIRED_JSON_KEY, QString::number(maxFramesOverDesired));
        }

        const QString WINDOW_STARVE_THRESHOLD_JSON_KEY = "window_starve_threshold";
        int windowStarveThreshold = audioBufferGroupObject[WINDOW_STARVE_THRESHOLD_JSON_KEY].toString().toInt(&ok);
        if (ok && windowStarveThreshold != InboundAudioStream::WINDOW_STARVE_THRESHOLD) {
            deprecationNotice(WINDOW_STARVE_THRESHOLD_JSON_KEY, QString::number(windowStarveThreshold));
        }

        const QString WINDOW_SECONDS_FOR_DESIRED_CALC_ON_TOO_MANY_STARVES_JSON_KEY = "window_seconds_for_desired_calc_on_too_many_starves";
        int windowSecondsForDesiredCalcOnTooManyStarves = audioBufferGroupObject[WINDOW_SECONDS_FOR_DESIRED_CALC_ON_TOO_MANY_STARVES_JSON_KEY].toString().toInt(&ok);
        if (ok && windowSecondsForDesiredCalcOnTooManyStarves != InboundAudioStream::WINDOW_SECONDS_FOR_DESIRED_CALC_ON_TOO_MANY_STARVES) {
            deprecationNotice(WINDOW_SECONDS_FOR_DESIRED_CALC_ON_TOO_MANY_STARVES_JSON_KEY, QString::number(windowSecondsForDesiredCalcOnTooManyStarves));
        }

        const QString WINDOW_SECONDS_FOR_DESIRED_REDUCTION_JSON_KEY = "window_seconds_for_desired_reduction";
        int windowSecondsForDesiredReduction = audioBufferGroupObject[WINDOW_SECONDS_FOR_DESIRED_REDUCTION_JSON_KEY].toString().toInt(&ok);
        if (ok && windowSecondsForDesiredReduction != InboundAudioStream::WINDOW_SECONDS_FOR_DESIRED_REDUCTION) {
            deprecationNotice(WINDOW_SECONDS_FOR_DESIRED_REDUCTION_JSON_KEY, QString::number(windowSecondsForDesiredReduction));
        }

        const QString USE_STDEV_FOR_JITTER_JSON_KEY = "use_stdev_for_desired_calc";
        bool useStDevForJitterCalc = audioBufferGroupObject[USE_STDEV_FOR_JITTER_JSON_KEY].toBool();
        if (useStDevForJitterCalc != InboundAudioStream::USE_STDEV_FOR_JITTER) {
            deprecationNotice(USE_STDEV_FOR_JITTER_JSON_KEY, useStDevForJitterCalc ? "true" : "false");
        }

        const QString REPETITION_WITH_FADE_JSON_KEY = "repetition_with_fade";
        bool repetitionWithFade = audioBufferGroupObject[REPETITION_WITH_FADE_JSON_KEY].toBool();
        if (repetitionWithFade != InboundAudioStream::REPETITION_WITH_FADE) {
            deprecationNotice(REPETITION_WITH_FADE_JSON_KEY, repetitionWithFade ? "true" : "false");
        }
    }

    if (settingsObject.contains(AUDIO_ENV_GROUP_KEY)) {
        QJsonObject audioEnvGroupObject = settingsObject[AUDIO_ENV_GROUP_KEY].toObject();

        const QString CODEC_PREFERENCE_ORDER = "codec_preference_order";
        if (audioEnvGroupObject[CODEC_PREFERENCE_ORDER].isString()) {
            _codecPreferenceOrder = audioEnvGroupObject[CODEC_PREFERENCE_ORDER].toString();
            qDebug() << "Codec preference order changed to" << _codecPreferenceOrder;
        }

        const QString ATTENATION_PER_DOULING_IN_DISTANCE = "attenuation_per_doubling_in_distance";
        if (audioEnvGroupObject[ATTENATION_PER_DOULING_IN_DISTANCE].isString()) {
            bool ok = false;
            float attenuation = audioEnvGroupObject[ATTENATION_PER_DOULING_IN_DISTANCE].toString().toFloat(&ok);
            if (ok) {
                _attenuationPerDoublingInDistance = attenuation;
                qDebug() << "Attenuation per doubling in distance changed to" << _attenuationPerDoublingInDistance;
            }
        }

        const QString NOISE_MUTING_THRESHOLD = "noise_muting_threshold";
        if (audioEnvGroupObject[NOISE_MUTING_THRESHOLD].isString()) {
            bool ok = false;
            float noiseMutingThreshold = audioEnvGroupObject[NOISE_MUTING_THRESHOLD].toString().toFloat(&ok);
            if (ok) {
                _noiseMutingThreshold = noiseMutingThreshold;
                qDebug() << "Noise muting threshold changed to" << _noiseMutingThreshold;
            }
        }

        const QString AUDIO_ZONES = "zones";
        if (audioEnvGroupObject[AUDIO_ZONES].isObject()) {
            const QJsonObject& zones = audioEnvGroupObject[AUDIO_ZONES].toObject();

            const QString X_MIN = "x_min";
            const QString X_MAX = "x_max";
            const QString Y_MIN = "y_min";
            const QString Y_MAX = "y_max";
            const QString Z_MIN = "z_min";
            const QString Z_MAX = "z_max";
            foreach (const QString& zone, zones.keys()) {
                QJsonObject zoneObject = zones[zone].toObject();

                if (zoneObject.contains(X_MIN) && zoneObject.contains(X_MAX) && zoneObject.contains(Y_MIN) &&
                    zoneObject.contains(Y_MAX) && zoneObject.contains(Z_MIN) && zoneObject.contains(Z_MAX)) {

                    float xMin, xMax, yMin, yMax, zMin, zMax;
                    bool ok, allOk = true;
                    xMin = zoneObject.value(X_MIN).toString().toFloat(&ok);
                    allOk &= ok;
                    xMax = zoneObject.value(X_MAX).toString().toFloat(&ok);
                    allOk &= ok;
                    yMin = zoneObject.value(Y_MIN).toString().toFloat(&ok);
                    allOk &= ok;
                    yMax = zoneObject.value(Y_MAX).toString().toFloat(&ok);
                    allOk &= ok;
                    zMin = zoneObject.value(Z_MIN).toString().toFloat(&ok);
                    allOk &= ok;
                    zMax = zoneObject.value(Z_MAX).toString().toFloat(&ok);
                    allOk &= ok;

                    if (allOk) {
                        glm::vec3 corner(xMin, yMin, zMin);
                        glm::vec3 dimensions(xMax - xMin, yMax - yMin, zMax - zMin);
                        AABox zoneAABox(corner, dimensions);
                        _audioZones.insert(zone, zoneAABox);
                        qDebug() << "Added zone:" << zone << "(corner:" << corner
                                    << ", dimensions:" << dimensions << ")";
                    }
                }
            }
        }

        const QString ATTENUATION_COEFFICIENTS = "attenuation_coefficients";
        if (audioEnvGroupObject[ATTENUATION_COEFFICIENTS].isArray()) {
            const QJsonArray& coefficients = audioEnvGroupObject[ATTENUATION_COEFFICIENTS].toArray();

            const QString SOURCE = "source";
            const QString LISTENER = "listener";
            const QString COEFFICIENT = "coefficient";
            for (int i = 0; i < coefficients.count(); ++i) {
                QJsonObject coefficientObject = coefficients[i].toObject();

                if (coefficientObject.contains(SOURCE) &&
                    coefficientObject.contains(LISTENER) &&
                    coefficientObject.contains(COEFFICIENT)) {

                    ZoneSettings settings;

                    bool ok;
                    settings.source = coefficientObject.value(SOURCE).toString();
                    settings.listener = coefficientObject.value(LISTENER).toString();
                    settings.coefficient = coefficientObject.value(COEFFICIENT).toString().toFloat(&ok);

                    if (ok && settings.coefficient >= 0.0f && settings.coefficient <= 1.0f &&
                        _audioZones.contains(settings.source) && _audioZones.contains(settings.listener)) {

                        _zoneSettings.push_back(settings);
                        qDebug() << "Added Coefficient:" << settings.source << settings.listener << settings.coefficient;
                    }
                }
            }
        }

        const QString REVERB = "reverb";
        if (audioEnvGroupObject[REVERB].isArray()) {
            const QJsonArray& reverb = audioEnvGroupObject[REVERB].toArray();

            const QString ZONE = "zone";
            const QString REVERB_TIME = "reverb_time";
            const QString WET_LEVEL = "wet_level";
            for (int i = 0; i < reverb.count(); ++i) {
                QJsonObject reverbObject = reverb[i].toObject();

                if (reverbObject.contains(ZONE) &&
                    reverbObject.contains(REVERB_TIME) &&
                    reverbObject.contains(WET_LEVEL)) {

                    bool okReverbTime, okWetLevel;
                    QString zone = reverbObject.value(ZONE).toString();
                    float reverbTime = reverbObject.value(REVERB_TIME).toString().toFloat(&okReverbTime);
                    float wetLevel = reverbObject.value(WET_LEVEL).toString().toFloat(&okWetLevel);

                    if (okReverbTime && okWetLevel && _audioZones.contains(zone)) {
                        ReverbSettings settings;
                        settings.zone = zone;
                        settings.reverbTime = reverbTime;
                        settings.wetLevel = wetLevel;

                        _zoneReverbSettings.push_back(settings);

                        qDebug() << "Added Reverb:" << zone << reverbTime << wetLevel;
                    }
                }
            }
        }
    }
}

AudioMixer::Timer::Timing::Timing(uint64_t& sum) : _sum(sum) {
    _timing = p_high_resolution_clock::now();
}

AudioMixer::Timer::Timing::~Timing() {
    _sum += std::chrono::duration_cast<std::chrono::microseconds>(p_high_resolution_clock::now() - _timing).count();
}

void AudioMixer::Timer::get(uint64_t& timing, uint64_t& trailing) {
    // update history
    _index = (_index + 1) % TIMER_TRAILING_SECONDS;
    uint64_t oldTiming = _history[_index];
    _history[_index] = _sum;

    // update trailing
    _trailing -= oldTiming;
    _trailing += _sum;

    timing = _sum;
    trailing = _trailing / TIMER_TRAILING_SECONDS;

    // reset _sum;
    _sum = 0;
}
