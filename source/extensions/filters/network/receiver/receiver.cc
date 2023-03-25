#include "source/extensions/filters/network/receiver/receiver.h"

#include "envoy/buffer/buffer.h"
#include "envoy/network/connection.h"

#include "source/common/common/assert.h"

#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace Receiver {

// Event called when receiving new client connection
Network::FilterStatus ReceiverFilter::onNewConnection() {
    ENVOY_LOG(debug, "onNewConnection triggered");
    return Network::FilterStatus::Continue;
}

// Event called when receiving data from the downstream proxy (through the listener)
Network::FilterStatus ReceiverFilter::onData(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "receiver: got {} bytes", read_callbacks_->connection(), data.length());

    // Downstream proxy closed the connection
    if ((end_stream == true || data.length() == 0) && !connection_close_) {
        ENVOY_LOG(info, "Downstream proxy closed the connection");
        // Terminate threads and connections
        close_procedure();
        // Do not go further in the filter chain
        return Network::FilterStatus::StopIteration;
    }

    // Executed at connection initialization with the downstream proxy (receives the RDMA port from it)
    else if (connection_init_) {
        ENVOY_LOG(info, "Connection init");
        ENVOY_LOG(debug, "read data: {}, end_stream: {}", data.toString(), end_stream);
        // Receive the port number of RDMA socket from downstream proxy
        std::string port = data.toString();
        ENVOY_LOG(debug, "port: {}", port);

        // Connect to RDMA downstream using the received port
        struct sockaddr_in downstream_address_;
        uint32_t downstream_port = std::stoul(port);
        std::string downstream_ip = read_callbacks_->connection().streamInfo().downstreamAddressProvider().remoteAddress()->ip()->addressAsString();
        downstream_address_.sin_family = AF_INET;
        downstream_address_.sin_port = htons(downstream_port);
        ENVOY_LOG(debug, "DOWNSTREAM IP: {}, Port: {}", downstream_ip, downstream_port);
        inet_pton(AF_INET, downstream_ip.c_str(), &downstream_address_.sin_addr);

        // Try to connect to RDMA downstream (try for 10 seconds)
        int count = 0;
        sock_rdma_ = socket(AF_INET, SOCK_STREAM, 0);
        while ((connect(sock_rdma_, reinterpret_cast<struct sockaddr*>(&downstream_address_), sizeof(downstream_address_ ))) != 0) {
            if (count >= 10) {
                ENVOY_LOG(error, "RDMA Failed to connect to downstream");
                return Network::FilterStatus::StopIteration;
            }
            ENVOY_LOG(info, "RETRY CONNECTING TO RDMA DOWNSTREAM...");
            sleep(1);
            count++;
        }
        ENVOY_LOG(info, "CONNECTED TO RDMA DOWNSTREAM");

        // Launch RDMA polling thread
        rdma_polling_thread_ = thread_factory_.createThread([this]() {this->rdma_polling();}, absl::nullopt);

        // Launch RDMA sender thread
        rdma_sender_thread_ = thread_factory_.createThread([this]() {this->rdma_sender();}, absl::nullopt);

        // Launch upstream sender thread
        upstream_sender_thread_ = thread_factory_.createThread([this]() {this->upstream_sender();}, absl::nullopt);
        
        // Connection init is now done
        connection_init_ = false;
    }

    // Drain read buffer
    data.drain(data.length());

    // Do not go further in the filter chain
    return Network::FilterStatus::StopIteration;
} 

// Event called when receiving data from the server (through tcp_proxy)
Network::FilterStatus ReceiverFilter::onWrite(Buffer::Instance& data, bool end_stream) {
    ENVOY_CONN_LOG(trace, "receiver: got {} bytes", write_callbacks_->connection(), data.length());
    ENVOY_LOG(debug, "written data: {}, end_stream: {}", data.toString(), end_stream);

    // Server closed the connection
    if (end_stream == true && !connection_close_) {
        ENVOY_LOG(info, "Server closed the connection");

        // if (data.length() != 0) {
        //     // Push last message followed by the end message
        //     push(upstream_to_downstream_buffer_, data.toString());
        // }
        // std::string end = "end";
        // push(upstream_to_downstream_buffer_, end);

        // Terminate threads and close filter connections
        close_procedure();

        // Drain write buffer
        data.drain(data.length());

        // End message to signal downstream proxy to close connection
        Buffer::OwnedImpl buff_end("end"); // End message to signal downstream proxy to close connection
        data.add(buff_end);

        // End message propagated to the downstream proxy through the listener
        return Network::FilterStatus::Continue;
    }
    
    // Push received data to circular buffer
    push(upstream_to_downstream_buffer_, data.toString());

    // Drain write buffer
    data.drain(data.length());

    // Do not go further in the filter chain
    return Network::FilterStatus::StopIteration;
} 

} // namespace Receiver
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
