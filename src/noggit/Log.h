// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <ios>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>

namespace Noggit::Logging
{
  enum class Channel
  {
    Standard,
    Debug,
    Error
  };

  //! Collects one log line and writes it to the log as a single block.
  //!
  //! Noggit loads client data from several threads, and those threads all write to
  //! the same redirected stream. Writing straight to std::cout let the lines
  //! interleave in the middle of a message, which made log.txt unreadable exactly
  //! where it matters most (during a crash). Buffering the line and handing it over
  //! under a mutex keeps every entry in one piece, and flushing it from the
  //! destructor means the last line before a crash is never lost.
  class Line
  {
  public:
    Line(Channel channel, char const* file, int line);
    ~Line();

    Line(Line const&) = delete;
    Line& operator=(Line const&) = delete;
    Line(Line&& other) noexcept;
    Line& operator=(Line&& other) noexcept;

    template<typename T>
    Line& operator<<(T const& value)
    {
      if (_buffer)
        *_buffer << value;
      return *this;
    }

    //! std::endl, std::flush, ...
    Line& operator<<(std::ostream& (*manipulator)(std::ostream&))
    {
      if (_buffer)
        *_buffer << manipulator;
      return *this;
    }

    //! std::hex, std::setprecision, ...
    Line& operator<<(std::ios_base& (*manipulator)(std::ios_base&))
    {
      if (_buffer)
        *_buffer << manipulator;
      return *this;
    }

  private:
    void flush();

    Channel _channel;
    std::string _file;
    int _line;
    long long _timestamp;
    std::unique_ptr<std::ostringstream> _buffer;
  };

  Line line(Channel channel, char const* file, int line);
}

#define LogError ::Noggit::Logging::line(::Noggit::Logging::Channel::Error, __FILE__, __LINE__)
#define LogDebug ::Noggit::Logging::line(::Noggit::Logging::Channel::Debug, __FILE__, __LINE__)
#define Log ::Noggit::Logging::line(::Noggit::Logging::Channel::Standard, __FILE__, __LINE__)

void InitLogging();
