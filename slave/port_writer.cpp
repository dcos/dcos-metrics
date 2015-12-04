#include "port_writer.hpp"
#include "sync_util.hpp"

#include <glog/logging.h>

#define UDP_MAX_PACKET_BYTES 65536 /* UDP size limit in IPv4 (may be larger in IPv6) */

typedef boost::asio::ip::udp::resolver resolver_t;

namespace stats {
  size_t select_buffer_capacity(const mesos::Parameters& parameters) {
    size_t params_size =
      params::get_uint(parameters, params::CHUNK_SIZE_BYTES, params::CHUNK_SIZE_BYTES_DEFAULT);
    if (params_size == 0) {
      LOG(WARNING) << "Ignoring invalid requested UDP packet size " << params_size << ", "
                   << "using " << params::CHUNK_SIZE_BYTES_DEFAULT;
      return params::CHUNK_SIZE_BYTES_DEFAULT;
    }
    if (params_size > UDP_MAX_PACKET_BYTES) {
      LOG(WARNING) << "Ignoring excessive requested UDP packet size " << params_size << ", "
                   << "using " << UDP_MAX_PACKET_BYTES;
      return UDP_MAX_PACKET_BYTES;
    }
    return params_size;
  }

  bool add_to_buffer(char* buffer, size_t buffer_capacity, size_t& buffer_used,
      const char* to_add, size_t to_add_size) {
    if (to_add_size == 0) {
      return true;
    }
    if (buffer_used == 0) {
      // Buffer is empty, no separator is needed yet.
      if (to_add_size > buffer_capacity) {
        return false;// Too big
      }
    } else {
      // Appending to existing buffer, with newline char as separator.
      if ((buffer_used + to_add_size + 1) > buffer_capacity) {// "1" includes newline char
        return false;// Too big
      }
      // Insert a newline separator before adding the new row
      buffer[buffer_used] = '\n';
      ++buffer_used;
    }
    // Add the new data following any existing data.
    memcpy(buffer + buffer_used, to_add, to_add_size);
    buffer_used += to_add_size;
    std::string tmp(buffer, buffer_used);
    for (int i = 0; i < tmp.size(); ++i) {
      if (tmp[i] == '\n') {
        tmp[i] = '\\';
      }
    }
    return true;
  }
}

stats::PortWriter::PortWriter(std::shared_ptr<boost::asio::io_service> io_service,
    const mesos::Parameters& parameters,
    size_t chunk_timeout_ms/*=1000*/)
  : send_host(params::get_str(parameters, params::DEST_HOST, params::DEST_HOST_DEFAULT)),
    send_port(params::get_uint(parameters, params::DEST_PORT, params::DEST_PORT_DEFAULT)),
    buffer_capacity(select_buffer_capacity(parameters)),
    chunking(params::get_bool(parameters, params::CHUNKING, params::CHUNKING_DEFAULT)),
    chunk_timeout_ms(chunk_timeout_ms),
    io_service(io_service),
    flush_timer(*io_service),
    socket(*io_service),
    buffer((char*) malloc(buffer_capacity)),
    buffer_used(0),
    shutdown(false) {
  LOG(INFO) << "Writer constructed for " << send_host << ":" << send_port;
}

stats::PortWriter::~PortWriter() {
  LOG(INFO) << "Asynchronously triggering PortWriter shutdown for "
            << send_host << ":" << send_port;
  if (sync_util::dispatch_run(
          "~PortWriter", *io_service, std::bind(&PortWriter::shutdown_cb, this))) {
    LOG(INFO) << "PortWriter shutdown succeeded";
  } else {
    LOG(ERROR) << "Failed to complete PortWriter shutdown for " << send_host << ":" << send_port;
  }
}

/* TODO:
 * instead of explicitly running open() once up-front, run a timer which re-resolves the host every
 * few seconds/minutes and automatically resets to a new random destination if the current
 * destination is no longer listed. this also would allow for scenarios where the host fails lookup
 * at first but starts working later */
Try<Nothing> stats::PortWriter::open() {
  if (socket.is_open()) {
    return Nothing();
  }

  resolver_t resolver(*io_service);
  resolver_t::query query(send_host, "");
  boost::system::error_code ec;
  resolver_t::iterator iter = resolver.resolve(query, ec);
  boost::asio::ip::address resolved_address;
  if (ec) {
    // failed, fall back to using the host as-is
    LOG(WARNING) << "Error when resolving host[" << send_host << "], trying to use as ip: " << ec;
    resolved_address = boost::asio::ip::address::from_string(send_host, ec);
    if (ec) {
      //FIXME: Retry loop instead of exiting process?
      LOG(WARNING) << "Error when resolving configured output host[" << send_host << "]: " << ec;
    }
  } else if (iter == resolver_t::iterator()) {
    // no results, fall back to using the host as-is
    LOG(WARNING) << "No results when resolving host[" << send_host << "], trying to use as ip.";
    resolved_address = boost::asio::ip::address::from_string(send_host, ec);
    if (ec) {
      //FIXME: Retry loop instead of exiting process?
      LOG(FATAL) << "No results when resolving configured output host[" << send_host << "]: " << ec;
    }
  } else {
    // resolved, bind output socket to first entry in list
    //TODO: select random entry, to support round-robin across A records
    resolved_address = iter->endpoint().address();
    LOG(INFO) << "Resolved dest_host[" << send_host << "] -> " << resolved_address;
  }

  send_endpoint = udp_endpoint_t(resolved_address, send_port);
  socket.open(send_endpoint.protocol(), ec);
  if (ec) {
    std::ostringstream oss;
    oss << "Failed to open writer socket to endpoint[" << send_endpoint << "]: " << ec;
    return Try<Nothing>::error(oss.str());
  }

  start_chunk_flush_timer();

  return Nothing();
}

void stats::PortWriter::write(const char* bytes, size_t size) {
  if (chunking) {
    if (add_to_buffer(buffer, buffer_capacity, buffer_used, bytes, size)) {
      // Data added to buffer. Will be flushed when buffer is full or when flush timer goes off.
      return;
    }

    // New data doesn't fit in the buffer's available space. Send/empty the buffer and try again.
    send_raw_bytes(buffer, buffer_used);
    buffer_used = 0;
    if (add_to_buffer(buffer, buffer_capacity, buffer_used, bytes, size)) {
      // Data now fit in buffer after freeing space with a manual flush. To be flushed again later.
      return;
    }

    // Data is too big to fit in an empty buffer. Skip the buffer and send the data as-is (below).
    // Note that the data doesn't jump the 'queue' since we've already sent the contents of the
    // current buffer.
    LOG(WARNING) << "Ignoring requested packet max[" << buffer_capacity << "]: "
                 << "Sending metric of size " << size;
  }

  send_raw_bytes(bytes, size);
}

void stats::PortWriter::start_chunk_flush_timer() {
  if (chunking) {
    flush_timer.expires_from_now(boost::posix_time::milliseconds(chunk_timeout_ms));
    flush_timer.async_wait(std::bind(&PortWriter::chunk_flush_cb, this, std::placeholders::_1));
  } else {
    // Ensure that the io_service always has some work to do, otherwise io_service.run() will return
    // immediately without performing any work. This hack is effectively a no-op since the buffer
    // will always be unused/empty when chunk_flush_cb is invoked.
    //TODO this stub timer can be removed once the re-resolve timer is added. See TODO above.
    flush_timer.expires_from_now(boost::posix_time::hours(24));
    flush_timer.async_wait(std::bind(&PortWriter::chunk_flush_cb, this, std::placeholders::_1));
  }
}

void stats::PortWriter::chunk_flush_cb(boost::system::error_code ec) {
  DLOG(INFO) << "Flush triggered";
  if (ec) {
    LOG(ERROR) << "Flush timer returned error:" << ec;
    if (boost::asio::error::operation_aborted) {
      // We're being destroyed. Don't look at local state, it may be destroyed already.
      LOG(WARNING) << "Aborted: Exiting write loop immediately";
      return;
    }
  }

  send_raw_bytes(buffer, buffer_used);
  buffer_used = 0;

  start_chunk_flush_timer();
}

void stats::PortWriter::send_raw_bytes(const char* bytes, size_t size) {
  if (size == 0) {
    DLOG(INFO) << "Skipping scheduled send of zero bytes";
    return;
  }

  if (!socket.is_open()) {
    LOG(WARNING) << "Dropped " << size << " bytes of data due to lack of open writer socket to "
                 << send_host << ":" << send_port;
    return;
  }

  DLOG(INFO) << "Send " << size << " bytes to " << send_host << ":" << send_port;
  boost::system::error_code ec;
  size_t sent = socket.send_to(boost::asio::buffer(bytes, size), send_endpoint, 0 /* flags */, ec);
  if (ec) {
    LOG(ERROR) << "Failed to send " << size << " bytes of data to "
               << send_host << ":" << send_port << ": " << ec;
  }
  if (sent != size) {
    LOG(WARNING) << "Sent size=" << sent << " doesn't match requested size=" << size;
  }
}

void stats::PortWriter::shutdown_cb() {
  boost::system::error_code ec;
  if (socket.is_open()) {
    chunk_flush_cb(boost::system::error_code());
    socket.close(ec);
    if (ec) {
      LOG(ERROR) << "Error on writer socket close: " << ec;
    }
  }
  free(buffer);
  buffer = NULL;
}
