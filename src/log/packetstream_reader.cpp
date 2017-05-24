/* This file is part of the Pangolin Project.
 * http://github.com/stevenlovegrove/Pangolin
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <pangolin/log/packetstream_reader.h>

using std::string;
using std::istream;
using std::ios;
using std::lock_guard;
using std::runtime_error;
using std::ios_base;
using std::streampos;
using std::streamoff;

#include <thread>

#ifndef _WIN_
#  include <unistd.h>
#endif

namespace pangolin
{

PacketStreamReader::PacketStreamReader()
    : _pipe_fd(-1)
{
}

PacketStreamReader::PacketStreamReader(const std::string& filename)
    : _pipe_fd(-1)
{
    Open(filename);
}

PacketStreamReader::~PacketStreamReader()
{
    Close();
}

void PacketStreamReader::Open(const std::string& filename)
{
    std::lock_guard<std::recursive_mutex> lg(_mutex);

    Close();

    _filename = filename;
    _is_pipe = IsPipe(filename);
    _stream.open(filename);

    if (!_stream.is_open())
        throw runtime_error("Cannot open stream.");

    for (auto i : PANGO_MAGIC)
    {
        if (_stream.get() != i)
            throw runtime_error("Unrecognised file header.");
        if (!_stream.good())
            throw runtime_error("Bad stream");
    }

    SetupIndex();

    ParseHeader();

    while (_stream.peekTag() == TAG_ADD_SOURCE) {
        ParseNewSource();
    }
}

void PacketStreamReader::Close() {
    std::lock_guard<std::recursive_mutex> lg(_mutex);

    _stream.close();
    _sources.clear();

#ifndef _WIN_
    if (_pipe_fd != -1) {
        close(_pipe_fd);
    }
#endif
}

void PacketStreamReader::ParseHeader()
{
    _stream.readTag(TAG_PANGO_HDR);

    picojson::value json_header;
    picojson::parse(json_header, _stream);

    // File timestamp
    const int64_t start_us = json_header["time_us"].get<int64_t>();
    packet_stream_start = SyncTime::TimePoint() + std::chrono::microseconds(start_us);

    _stream.get(); // consume newline
}

void PacketStreamReader::ParseNewSource()
{
    _stream.readTag(TAG_ADD_SOURCE);
    picojson::value json;
    picojson::parse(json, _stream);
    _stream.get(); // consume newline

    PacketStreamSource pss;
    pss.driver = json[pss_src_driver].get<string>();
    pss.id = json[pss_src_id].get<int64_t>();
    pss.uri = json[pss_src_uri].get<string>();
    pss.info = json[pss_src_info];
    pss.version = json[pss_src_version].get<int64_t>();
    pss.data_alignment_bytes = json[pss_src_packet][pss_pkt_alignment_bytes].get<int64_t>();
    pss.data_definitions = json[pss_src_packet][pss_pkt_definitions].get<string>();
    pss.data_size_bytes = json[pss_src_packet][pss_pkt_size_bytes].get<int64_t>();

    if (_sources.size() != pss.id)
        throw runtime_error("Id mismatch parsing source descriptors. Possible corrupt stream?");
    _sources.push_back(pss);
    if (_next_packet_framenum.size() < pss.id + 1)
        _next_packet_framenum.resize(pss.id + 1);
    _next_packet_framenum[pss.id] = 0;
}

void PacketStreamReader::SetupIndex()
{
    if (!_stream.seekable())
        return;

    auto pos = _stream.tellg();
    _stream.seekg(-(static_cast<istream::off_type>(sizeof(uint64_t)) + TAG_LENGTH), ios_base::end); //a footer is a tag + index position. //todo: this will choke on trailing whitespace. Make it not choke on trailing whitespace.
    //todo also: this will break if the footer size changes. Make this more dynamic.

    if (_stream.peekTag() == TAG_PANGO_FOOTER)
    {
        _stream.seekg(ParseFooter()); //parsing the footer returns the index position
        if (_stream.peekTag() == TAG_PANGO_STATS)
            ParseIndex();
    }

    _stream.clear();
    _stream.seekg(pos);
}

streampos PacketStreamReader::ParseFooter() //returns position of index.
{
    _stream.readTag(TAG_PANGO_FOOTER);
    uint64_t index;
    _stream.read(reinterpret_cast<char*>(&index), sizeof(index));
    return index;
}

void PacketStreamReader::ParseIndex()
{
    _stream.readTag(TAG_PANGO_STATS);
    picojson::value json;
    picojson::parse(json, _stream);

    if (json.contains("src_packet_index")) //this is a two-dimensional serialized array, [source id][sequence number] ---> packet position in stream
    {
        const auto& json_index = json["src_packet_index"].get<picojson::array>();  //reference to the whole array
        _index = PacketIndex(json_index);
    }
}

bool PacketStreamReader::GoodToRead()
{
    if(!_stream.good()) {
#ifndef _WIN_
        if (_is_pipe)
        {
            if (_pipe_fd == -1) {
                _pipe_fd = ReadablePipeFileDescriptor(_filename);
            }

            if (_pipe_fd != -1) {
                // Test whether the pipe has data to be read. If so, open the
                // file stream and start reading. After this point, the file
                // descriptor is owned by the reader.
                if (PipeHasDataToRead(_pipe_fd))
                {
                    close(_pipe_fd);
                    _pipe_fd = -1;
                    Open(_filename);
                    return _stream.good();
                }
            }
        }
#endif

        return false;
    }

    return true;

}

FrameInfo PacketStreamReader::_nextFrame()
{
    while (GoodToRead())
    {
        auto t = _stream.peekTag();

        switch (t)
        {
        case TAG_PANGO_SYNC:
            SkipSync();
            break;
        case TAG_ADD_SOURCE:
            ParseNewSource();
            break;
        case TAG_SRC_JSON: //frames are sometimes preceded by metadata, but metadata must ALWAYS be followed by a frame from the same source.
        case TAG_SRC_PACKET:
            return readFrameHeader();
        case TAG_PANGO_STATS:
            ParseIndex();
            break;
        case TAG_PANGO_FOOTER: //end of frames
        case TAG_END:
            return FrameInfo(); //none
        case TAG_PANGO_HDR: //shoudln't encounter this
            ParseHeader();
            break;
        case TAG_PANGO_MAGIC: //or this
            SkipSync();
            break;
        default: //or anything else
            pango_print_warn("Unexpected packet type: \"%s\". Resyncing()\n", tagName(t).c_str());
            ReSync();
            break;
        }
    }

    // No frame
    return FrameInfo();
}

FrameInfo PacketStreamReader::NextFrame(PacketStreamSourceId src)
{
    Lock(); //we cannot use a scoped lock here, because we may not want to release the lock, depending on what we find.
    try
    {
        while (1)
        {
            auto fi = _nextFrame();
            if (!fi) {
                // Nothing left in stream
                Unlock();
                return fi;
            } else {
                // So we have accurate sequence numbers for frames.
                ++_next_packet_framenum[fi.src];

                if (_stream.seekable())
                {
                    if (!_index.has(fi.src, fi.sequence_num)) {
                        // If it's not in the index for some reason, add it.
                        _index.add(fi.src, fi.sequence_num, fi.frame_streampos);
                    } else if (_index.position(fi.src, fi.sequence_num) != fi.frame_streampos) {
                        PANGO_ENSURE(_index.position(fi.src, fi.sequence_num) == fi.packet_streampos);
                        static bool warned_already = false;
                        if(!warned_already) {
                            pango_print_warn("CAUTION: Old .pango files do not update frame_properties on seek.\n");
                            warned_already = true;
                        }
                    }
                }
                _stream.data_len(fi.size); //now we are positioned on packet data for n characters.
            }

            //if it's ours, return it and don't release lock
            if (fi.src == src) {
                return fi;
            }

            //otherwise skip it and get the next one.
            _stream.skip(fi.size);
        }
    }
    catch (std::exception &e) {
        // Since we are not using a scoped lock, we must catch and release.
        Unlock();
        throw e;
    }
    catch (...)  {
        // We will always release, even if we cannot identify the exception.
        Unlock();
        throw std::runtime_error("Caught an unknown exception");
    }
}

size_t PacketStreamReader::ReadRaw(char* target, size_t len)
{
    if (!_stream.data_len()) {
        throw runtime_error("Packetstream not positioned on data block. nextFrame() should be called before readraw().");
    } else if (_stream.data_len() < len) {
        pango_print_warn("readraw() requested read of %zu bytes when only %zu bytes remain in data block. Trimming to available data size.", len, _stream.data_len());
        len = _stream.data_len();
    }

    auto r = _stream.read(target, len);

    if (!_stream.data_len()) {
        //we are done reading, and should release the lock from nextFrame()
        Unlock();
    }

    return r;
}

size_t PacketStreamReader::Skip(size_t len)
{
    if (!_stream.data_len())
        throw runtime_error("Packetstream not positioned on data block. nextFrame() should be called before skip().");
    else if (_stream.data_len() < len)
    {
        pango_print_warn("skip() requested skip of %zu bytes when only %zu bytes remain in data block. Trimming to remaining data size.", len, _stream.data_len());
        len = _stream.data_len();
    }
    auto r = _stream.skip(len);
    if (!_stream.data_len()) //we are done skipping, and should release the lock from nextFrame()
        Unlock();
    return r;
}

FrameInfo PacketStreamReader::Seek(PacketStreamSourceId src, size_t framenum)
{
    lock_guard<decltype(_mutex)> lg(_mutex);

    if (!_stream.seekable())
        throw std::runtime_error("Stream is not seekable (probably a pipe).");

    if (src > _sources.size())
        throw std::runtime_error("Invalid Frame Source ID.");

    if(_stream.data_len()) //we were in the middle of reading data, and are holding an extra lock. We need to release it, while still holding the scoped lock.
       Skip(_stream.data_len());

    while (!_index.has(src, framenum))
    {
        pango_print_warn("seek index miss... reading ahead.\n");

        if (_stream.data_len())
            _stream.skip(_stream.data_len());

        auto fi = NextFrame(src);
        if (!fi) //if we hit the end, throw
            throw std::out_of_range("frame number not in sequence");
    }

    auto target_header_start = _index.position(src, framenum);

    _stream.seekg(target_header_start);
    _next_packet_framenum[src] = framenum; //this increments when we parse the header in the next line;
    //THIS WILL BREAK _next_packet_framenum FOR ALL OTHER SOURCES. Todo more refactoring to fix.

    // Read header and rest back
    auto r = readFrameHeader();
    _stream.seekg(r.packet_streampos);

    return r;
}

void PacketStreamReader::SkipSync()
{
    //Assume we have just read PAN, read GO
    if (_stream.get() != 'G' && _stream.get() != 'O')
        throw std::runtime_error("Unknown packet type.");

    while (_stream.peekTag() != TAG_SRC_PACKET && _stream.peekTag() != TAG_END)
        _stream.readTag();
}

size_t PacketStreamReader::GetPacketIndex(PacketStreamSourceId src_id) const //returns the current frame for source
{
    return _next_packet_framenum.at(src_id);
}

FrameInfo PacketStreamReader::readFrameHeader()
{
    FrameInfo frame;

    frame.frame_streampos = _stream.tellg();

    if (_stream.peekTag() == TAG_SRC_JSON)
    {
        _stream.readTag(TAG_SRC_JSON);
        frame.src = _stream.readUINT();
        picojson::parse(frame.meta, _stream);
    }

    frame.packet_streampos = _stream.tellg();

    _stream.readTag(TAG_SRC_PACKET);
    frame.time = _stream.readTimestamp();

    if (frame) {
        if (_stream.readUINT() != frame.src) {
            throw std::runtime_error("Frame preceded by metadata for a mismatched source. Stream may be corrupt.");
        }
    } else {
        frame.src = _stream.readUINT();
    }

    frame.size = Sources()[frame.src].data_size_bytes;
    if (!frame.size)
        frame.size = _stream.readUINT();
    frame.sequence_num = GetPacketIndex(frame.src);

    return frame;
}

}

