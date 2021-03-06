//
// windows_sspi/stream.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2020 Kasper Laudrup (laudrup at stacktrace dot dk)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_WINDOWS_SSPI_STREAM_HPP
#define BOOST_ASIO_WINDOWS_SSPI_STREAM_HPP

#include "error.hpp"
#include "stream_base.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#define SECURITY_WIN32

#include <schannel.h>
#include <security.h>
#include <sspi.h>
#include <wincrypt.h>
#include <windows.h>
#include <wintrust.h>

#include <array>
#include <iterator>
#include <stdexcept>
#include <type_traits>

namespace boost {
namespace asio {
namespace windows_sspi {

template <typename NextLayer> class stream : public stream_base {
public:
  using next_layer_type = NextLayer;
  using lowest_layer_type = typename std::remove_reference<next_layer_type>::type::lowest_layer_type;

  template <typename Arg> stream(Arg &&arg, context &ctx) : stream_base(ctx), m_next_layer(std::forward<Arg>(arg)) {
  }

  ~stream() {
    m_context_impl->sspi_functions->DeleteSecurityContext(&m_security_context);
  }

  const lowest_layer_type &lowest_layer() const {
    return m_next_layer.lowest_layer();
  }

  lowest_layer_type &lowest_layer() {
    return m_next_layer.lowest_layer();
  }

  void handshake(handshake_type type) {
    DWORD flags_out = 0;
    DWORD flags_in = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                     ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

    boost::system::error_code ec;
    SECURITY_STATUS sc = SEC_E_OK;

    SecBufferDesc OutBuffer;
    SecBuffer OutBuffers[1];
    SecBufferDesc InBuffer;
    SecBuffer InBuffers[2];

    if (type == client) {
      OutBuffers[0].pvBuffer = NULL;
      OutBuffers[0].BufferType = SECBUFFER_TOKEN;
      OutBuffers[0].cbBuffer = 0;

      OutBuffer.cBuffers = 1;
      OutBuffer.pBuffers = OutBuffers;
      OutBuffer.ulVersion = SECBUFFER_VERSION;

      sc = m_context_impl->sspi_functions->InitializeSecurityContext(&m_context_impl->handle, NULL, NULL, flags_in, 0,
                                                                     SECURITY_NATIVE_DREP, NULL, 0, &m_security_context,
                                                                     &OutBuffer, &flags_out, NULL);
      if (sc != SEC_I_CONTINUE_NEEDED) {
        throw boost::system::system_error(error::make_error_code(sc), "InitializeSecurityContext");
      }

      size_t size_written =
        m_next_layer.write_some(boost::asio::const_buffer(OutBuffers[0].pvBuffer, OutBuffers[0].cbBuffer), ec);
      BOOST_ASSERT(size_written == OutBuffers[0].cbBuffer);
      m_context_impl->sspi_functions->FreeContextBuffer(OutBuffers[0].pvBuffer);
      if (ec) {
        throw boost::system::system_error(ec);
      }
    }

    size_t input_size = 0;
    std::array<char, 0x10000> buffer;

    while (true) {
      input_size += m_next_layer.read_some(boost::asio::buffer(buffer.data() + input_size, buffer.size() - input_size), ec);
      if (ec) {
        throw boost::system::system_error(ec);
      }

      InBuffers[0].pvBuffer = reinterpret_cast<void*>(buffer.data());
      InBuffers[0].cbBuffer = static_cast<ULONG>(input_size);
      InBuffers[0].BufferType = SECBUFFER_TOKEN;

      InBuffers[1].pvBuffer = NULL;
      InBuffers[1].cbBuffer = 0;
      InBuffers[1].BufferType = SECBUFFER_EMPTY;

      InBuffer.cBuffers = 2;
      InBuffer.pBuffers = InBuffers;
      InBuffer.ulVersion = SECBUFFER_VERSION;

      OutBuffers[0].pvBuffer = NULL;
      OutBuffers[0].BufferType = SECBUFFER_TOKEN;
      OutBuffers[0].cbBuffer = 0;

      OutBuffer.cBuffers = 1;
      OutBuffer.pBuffers = OutBuffers;
      OutBuffer.ulVersion = SECBUFFER_VERSION;

      sc = m_context_impl->sspi_functions->InitializeSecurityContext(
          &m_context_impl->handle, &m_security_context, NULL, flags_in, 0, SECURITY_NATIVE_DREP, &InBuffer, 0, NULL,
          &OutBuffer, &flags_out, NULL);

      if (OutBuffers[0].cbBuffer != 0 && OutBuffers[0].pvBuffer != NULL) {
        m_next_layer.write_some(boost::asio::const_buffer(OutBuffers[0].pvBuffer, OutBuffers[0].cbBuffer), ec);
        m_context_impl->sspi_functions->FreeContextBuffer(OutBuffers[0].pvBuffer);
        OutBuffers[0].pvBuffer = NULL;
        if (ec) {
          throw boost::system::system_error(ec);
        }
      }

      switch (sc) {
        case SEC_E_INCOMPLETE_MESSAGE:
          continue;

        case SEC_E_OK:
          BOOST_ASSERT_MSG(InBuffers[1].BufferType != SECBUFFER_EXTRA, "Handle extra data from handshake");
          return;

        case SEC_I_INCOMPLETE_CREDENTIALS:
          BOOST_ASSERT_MSG(false, "client authentication not implemented");

        default:
          if (FAILED(sc)) {
            throw boost::system::system_error(error::make_error_code(sc), "InitializeSecurityContext");
          }
      }

      if (InBuffers[1].BufferType == SECBUFFER_EXTRA) {
        std::copy_n(buffer.data() + (input_size - InBuffers[1].cbBuffer), InBuffers[1].cbBuffer, buffer.data());
        input_size = InBuffers[1].cbBuffer;
      } else {
        input_size = 0;
      }
    }
  }

  template <typename MutableBufferSequence>
  size_t read_some(const MutableBufferSequence &buffers, boost::system::error_code &ec) {
    while (m_received_data.empty()) {
      SecBufferDesc Message;
      SecBuffer Buffers[4];

      Buffers[0].pvBuffer = m_input_buffer.data();
      Buffers[0].cbBuffer = static_cast<ULONG>(m_input_size);
      Buffers[0].BufferType = SECBUFFER_DATA;
      Buffers[1].BufferType = SECBUFFER_EMPTY;
      Buffers[2].BufferType = SECBUFFER_EMPTY;
      Buffers[3].BufferType = SECBUFFER_EMPTY;

      Message.ulVersion = SECBUFFER_VERSION;
      Message.cBuffers = 4;
      Message.pBuffers = Buffers;

      SECURITY_STATUS sc = m_context_impl->sspi_functions->DecryptMessage(&m_security_context, &Message, 0, NULL);
      if (sc == SEC_E_INCOMPLETE_MESSAGE) {
        std::size_t size_read = m_next_layer.read_some(boost::asio::buffer(m_input_buffer.data() + m_input_size, m_input_buffer.size() - m_input_size), ec);
        if (ec) {
          return 0;
        }
        m_input_size += size_read;
        continue;
      }
      if (sc == SEC_I_CONTEXT_EXPIRED) {
        ec = boost::asio::error::eof;
        return 0;
      }
      if (FAILED(sc)) {
        ec = error::make_error_code(sc);
        return 0;
      }

      m_input_size = 0;
      for (int i = 1; i < 4; i++) {
        if (Buffers[i].BufferType == SECBUFFER_DATA) {
          SecBuffer* pDataBuffer = pDataBuffer = &Buffers[i];
          std::copy_n(reinterpret_cast<const char *>(pDataBuffer->pvBuffer), pDataBuffer->cbBuffer,
                      std::back_inserter(m_received_data));
        }
        if (Buffers[i].BufferType == SECBUFFER_EXTRA) {
          SecBuffer* pExtraBuffer = &Buffers[i];
          std::copy_n(reinterpret_cast<const char *>(pExtraBuffer->pvBuffer),
                      pExtraBuffer->cbBuffer, m_input_buffer.data());
          m_input_size += pExtraBuffer->cbBuffer;
        }
      }
    }
    std::size_t to_return = std::min(boost::asio::buffer_size(buffers), m_received_data.size());
    boost::asio::buffer_copy(buffers, boost::asio::buffer(m_received_data, to_return));
    m_received_data.erase(m_received_data.begin(), m_received_data.begin() + to_return);
    return to_return;
  }

  template <typename ConstBufferSequence>
  std::size_t write_some(const ConstBufferSequence &buffers, boost::system::error_code &ec) {
    SecBufferDesc Message;
    SecBuffer Buffers[4];

    SecPkgContext_StreamSizes stream_sizes;
    SECURITY_STATUS sc = m_context_impl->sspi_functions->QueryContextAttributes(&m_security_context, SECPKG_ATTR_STREAM_SIZES, &stream_sizes);
    if (sc != SEC_E_OK) {
      ec = error::make_error_code(sc);
      return 0;
    }

    const auto input_size = boost::asio::buffer_size(buffers);
    BOOST_ASSERT(input_size <= stream_sizes.cbMaximumMessage);
    std::vector<char> message(stream_sizes.cbHeader + input_size + stream_sizes.cbTrailer);

    Buffers[0].pvBuffer = message.data();
    Buffers[0].cbBuffer = stream_sizes.cbHeader;
    Buffers[0].BufferType = SECBUFFER_STREAM_HEADER;

    boost::asio::buffer_copy(boost::asio::buffer(message.data() + stream_sizes.cbHeader, input_size), buffers);
    Buffers[1].pvBuffer = message.data() + stream_sizes.cbHeader;
    Buffers[1].cbBuffer = static_cast<ULONG>(input_size);
    Buffers[1].BufferType = SECBUFFER_DATA;

    Buffers[2].pvBuffer = message.data() + stream_sizes.cbHeader + input_size;
    Buffers[2].cbBuffer = stream_sizes.cbTrailer;
    Buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;

    Buffers[3].pvBuffer = SECBUFFER_EMPTY;
    Buffers[3].cbBuffer = SECBUFFER_EMPTY;
    Buffers[3].BufferType = SECBUFFER_EMPTY;

    Message.ulVersion = SECBUFFER_VERSION;
    Message.cBuffers = 4;
    Message.pBuffers = Buffers;
    sc = m_context_impl->sspi_functions->EncryptMessage(&m_security_context, 0, &Message, 0);

    if (FAILED(sc)) {
      ec = error::make_error_code(sc);
      return 0;
    }

    auto sent = m_next_layer.write_some(boost::asio::buffer(message), ec);
    BOOST_ASSERT(sent == message.size());
    if (ec) {
      return 0;
    }

    return input_size;
  }

private:
  next_layer_type m_next_layer;
  std::array<char, 0x10000> m_input_buffer;
  std::size_t m_input_size = 0;
  std::vector<char> m_received_data;
  CtxtHandle m_security_context;
};

} // namespace windows_sspi
} // namespace asio
} // namespace boost

#endif // BOOST_ASIO_WINDOWS_SSPI_STREAM_HPP
