/*
 * Copyright (C) 2019  Christian Berger
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "cluon-complete.hpp"
#include "opendlv-standard-message-set.hpp"

#include <cstdint>
#include <cstring>
#include <ctime>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

int32_t main(int32_t argc, char **argv) {
    int32_t retCode{1};
    auto commandlineArguments = cluon::getCommandlineArguments(argc, argv);
    if ( (0 == commandlineArguments.count("name")) ||
         (0 == commandlineArguments.count("width")) ||
         (0 == commandlineArguments.count("height")) ) {
        std::cerr << argv[0] << " attaches to an XYZ-formatted image point cloud residing in a shared memory area to store (uncompressed) to a file." << std::endl;
        std::cerr << "Usage:   " << argv[0] << " --name=<name of shared memory area> --width=<width> --height=<height> [--verbose] [--id=<identifier in case of multiple instances] [--cid=<OpenDaVINCI session to include Envelopes from the specified CID in the recording>] [--rec=MyFile.rec] [--recsuffix=Suffix]" << std::endl;
        std::cerr << "         --cid:             CID of the OD4Session to receive Envelopes to include in the recording file" << std::endl;
        std::cerr << "         --id:              when using several instances, this identifier is used as senderStamp" << std::endl;
        std::cerr << "         --rec:             name of the recording file; default: YYYY-MM-DD_HHMMSS.rec" << std::endl;
        std::cerr << "         --recsuffix:       additional suffix to add to the .rec file" << std::endl;
        std::cerr << "         --name:            name of the shared memory area to attach" << std::endl;
        std::cerr << "         --width:           width of the frame" << std::endl;
        std::cerr << "         --height:          height of the frame" << std::endl;
        std::cerr << "         --verbose:         print encoding information" << std::endl;
        std::cerr << "Example: " << argv[0] << " --name=data --width=640 --height=480 --verbose" << std::endl;
    }
    else {
        auto getYYYYMMDD_HHMMSS = [](){
            cluon::data::TimeStamp now = cluon::time::now();

            const long int _seconds = now.seconds();
            struct tm *tm = localtime(&_seconds);

            uint32_t year = (1900 + tm->tm_year);
            uint32_t month = (1 + tm->tm_mon);
            uint32_t dayOfMonth = tm->tm_mday;
            uint32_t hours = tm->tm_hour;
            uint32_t minutes = tm->tm_min;
            uint32_t seconds = tm->tm_sec;

            std::stringstream sstr;
            sstr << year << "-" << ( (month < 10) ? "0" : "" ) << month << "-" << ( (dayOfMonth < 10) ? "0" : "" ) << dayOfMonth
                           << "_" << ( (hours < 10) ? "0" : "" ) << hours
                           << ( (minutes < 10) ? "0" : "" ) << minutes
                           << ( (seconds < 10) ? "0" : "" ) << seconds;

            std::string retVal{sstr.str()};
            return retVal;
        };

        const std::string NAME{commandlineArguments["name"]};
        const std::string RECSUFFIX{commandlineArguments["recsuffix"]};
        const uint32_t WIDTH{static_cast<uint32_t>(std::stoi(commandlineArguments["width"]))};
        const uint32_t HEIGHT{static_cast<uint32_t>(std::stoi(commandlineArguments["height"]))};
        const uint32_t CID{(commandlineArguments["cid"].size() != 0) ? static_cast<uint32_t>(std::stoi(commandlineArguments["cid"])) : 0};
        const uint32_t ID{(commandlineArguments["id"].size() != 0) ? static_cast<uint32_t>(std::stoi(commandlineArguments["id"])) : 0};
        const std::string NAME_RECFILE{(commandlineArguments["rec"].size() != 0) ? commandlineArguments["rec"] + RECSUFFIX : (getYYYYMMDD_HHMMSS() + RECSUFFIX + ".rec")};
        const bool VERBOSE{commandlineArguments.count("verbose") != 0};

        std::unique_ptr<cluon::SharedMemory> sharedMemory(new cluon::SharedMemory{NAME});
        if (sharedMemory && sharedMemory->valid()) {
            std::clog << "[opendlv-video-xyz-recorder]: Attached to '" << sharedMemory->name() << "' (" << sharedMemory->size() << " bytes); recording data to '" << NAME_RECFILE << "'" << std::endl;

            std::mutex recFileMutex{};
            std::fstream recFile(NAME_RECFILE.c_str(), std::ios::out|std::ios::binary|std::ios::trunc);
            if (recFile.good()) {
                cluon::data::TimeStamp before, after, afterWriting, sampleTimeStamp;

                // Interface to a running OpenDaVINCI session (ignoring any incoming Envelopes).
                std::unique_ptr<cluon::OD4Session> od4{nullptr};
                if (CID > 0) {
                    od4.reset(new cluon::OD4Session(CID,
                              [&recFileMutex, &recFile](cluon::data::Envelope &&envelope){
                                  std::lock_guard<std::mutex> lck(recFileMutex);
                                  std::string data{cluon::serializeEnvelope(std::move(envelope))};
                                  recFile.write(data.data(), data.size());
                                  recFile.flush();
                              }));
                }

                while (sharedMemory && sharedMemory->valid() && !cluon::TerminateHandler::instance().isTerminated.load()) {
                    // Wait for incoming frame.
                    sharedMemory->wait();

                    sampleTimeStamp = cluon::time::now();

                    int totalSize{0};
                    sharedMemory->lock();
                    {
                        // Read notification timestamp.
                        auto r = sharedMemory->getTimeStamp();
                        sampleTimeStamp = (r.first ? r.second : sampleTimeStamp);
                    }
                    std::string data{sharedMemory->data(), sharedMemory->size()};
                    sharedMemory->unlock();

                    if (0 < totalSize) {
                        opendlv::proxy::ImageReading ir;
                        ir.fourcc("xyz")
                          .width(WIDTH)
                          .height(HEIGHT)
                          .data(data);
                        {
                            cluon::data::Envelope envelope;
                            {
                                cluon::ToProtoVisitor protoEncoder;
                                {
                                    envelope.dataType(ir.ID());
                                    ir.accept(protoEncoder);
                                    envelope.serializedData(protoEncoder.encodedData());
                                    envelope.sent(cluon::time::now());
                                    envelope.sampleTimeStamp(sampleTimeStamp);
                                    envelope.senderStamp(ID);
                                }
                            }

                            std::lock_guard<std::mutex> lck(recFileMutex);
                            std::string serializedData{cluon::serializeEnvelope(std::move(envelope))};
                            recFile.write(serializedData.data(), serializedData.size());
                            recFile.flush();

                            if (VERBOSE) {
                                afterWriting = cluon::time::now();
                            }
                        }

                        if (VERBOSE) {
                            std::clog << "[opendlv-video-xyz-recorder]: XYZ frame saved." << std::endl;
                        }
                    }
                }
            }
            retCode = 0;
        }
        else {
            std::cerr << "[opendlv-video-xyz-recorder]: Failed to attach to shared memory '" << NAME << "'." << std::endl;
        }
    }
    return retCode;
}
