// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Log.h>

#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <ostream>
#include <streambuf>

namespace Noggit::Logging
{
  namespace
  {
    std::mutex& logMutex()
    {
      static std::mutex mutex;
      return mutex;
    }

    std::ostream& channelStream(Channel channel)
    {
      switch (channel)
      {
      case Channel::Error:
        return std::cerr;
      case Channel::Debug:
        return std::clog;
      default:
        return std::cout;
      }
    }

    char const* channelName(Channel channel)
    {
      switch (channel)
      {
      case Channel::Error:
        return "[Error] ";
      case Channel::Debug:
        return "[Debug] ";
      default:
        return "";
      }
    }

    char const* baseName(char const* path)
    {
      char const* last = path;
      for (char const* character = path; *character; ++character)
      {
        if (*character == '/' || *character == '\\')
          last = character + 1;
      }
      return last;
    }
  }

  Line::Line(Channel channel, char const* file, int line)
    : _channel(channel)
    , _file(file ? file : "")
    , _line(line)
    // widened before the multiplication: clock() * 1000 overflows a 32 bit
    // clock_t about 35 minutes into a session on MSVC
    , _timestamp(static_cast<long long>(clock()) * 1000 / CLOCKS_PER_SEC)
    , _buffer(std::make_unique<std::ostringstream>())
  {
  }

  Line::Line(Line&& other) noexcept
    : _channel(other._channel)
    , _file(std::move(other._file))
    , _line(other._line)
    , _timestamp(other._timestamp)
    , _buffer(std::move(other._buffer))
  {
    // the moved from line must not write a second copy
    other._buffer.reset();
  }

  Line& Line::operator=(Line&& other) noexcept
  {
    if (this != &other)
    {
      flush();

      _channel = other._channel;
      _file = std::move(other._file);
      _line = other._line;
      _timestamp = other._timestamp;
      _buffer = std::move(other._buffer);
      other._buffer.reset();
    }

    return *this;
  }

  Line::~Line()
  {
    // Never let a logging failure take the application down: destructors that
    // throw call std::terminate.
    try
    {
      flush();
    }
    catch (...)
    {
    }
  }

  void Line::flush()
  {
    if (!_buffer)
      return;

    std::string const content = _buffer->str();
    _buffer.reset();

    std::ostringstream prefix;
    prefix << _timestamp << " - (" << baseName(_file.c_str()) << ":" << _line << "): "
           << channelName(_channel);

    // One buffer write for the whole line: anything else lets another thread slip
    // a message in between the prefix and the text.
    std::string text = prefix.str() + content;
    if (text.empty() || text.back() != '\n')
      text += '\n';

    std::lock_guard<std::mutex> lock(logMutex());

    auto* buffer = channelStream(_channel).rdbuf();
    if (!buffer)
      return;

    buffer->sputn(text.data(), static_cast<std::streamsize>(text.size()));
    buffer->pubsync();
  }

  Line line(Channel channel, char const* file, int line)
  {
    return Line(channel, file, line);
  }
}

#if DEBUG__LOGGINGTOCONSOLE
void InitLogging()
{
  LogDebug << "Logging to console window." << std::endl;
}
#else
namespace
{
  //! Serializes writes to the log file. Anything that writes to std::cout,
  //! std::clog or std::cerr - including the third party database and archive
  //! libraries - ends up here, and those writes come from several threads.
  class SynchronizedLogBuf : public std::streambuf
  {
  public:
    explicit SynchronizedLogBuf(std::ofstream& target)
      : _target(target)
    {
    }

    SynchronizedLogBuf(SynchronizedLogBuf const&) = delete;
    SynchronizedLogBuf& operator=(SynchronizedLogBuf const&) = delete;

  protected:
    int_type overflow(int_type character) override
    {
      if (character == traits_type::eof())
        return character;

      std::lock_guard<std::mutex> lock(_mutex);

      auto const value = traits_type::to_char_type(character);
      _target.put(value);

      // keep the file current: a crash right after this line must not lose it
      if (value == '\n')
        _target.flush();

      return character;
    }

    std::streamsize xsputn(char const* data, std::streamsize count) override
    {
      if (count <= 0)
        return 0;

      std::lock_guard<std::mutex> lock(_mutex);

      _target.write(data, count);

      if (std::memchr(data, '\n', static_cast<std::size_t>(count)) != nullptr)
        _target.flush();

      return count;
    }

    int sync() override
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _target.flush();
      return 0;
    }

  private:
    std::ofstream& _target;
    std::mutex _mutex;
  };

  // Deliberately leaked: std::cout keeps pointing at this buffer for as long as
  // the process lives, including during static destruction, so it must never be
  // destroyed before the streams that use it.
  std::ofstream& logStream()
  {
    static std::ofstream* stream = new std::ofstream();
    return *stream;
  }

  SynchronizedLogBuf& logBuffer()
  {
    static SynchronizedLogBuf* buffer = new SynchronizedLogBuf(logStream());
    return *buffer;
  }
}

void InitLogging()
{
  // Set up log.
  auto& stream = logStream();
  stream.open("log.txt", std::ios_base::out | std::ios_base::trunc);
  if (stream)
  {
    auto* buffer = &logBuffer();
    std::cout.rdbuf(buffer);
    std::clog.rdbuf(buffer);
    std::cerr.rdbuf(buffer);
  }
}
#endif
