/*
 * Copyright (c) 2013, Peter Thorson. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the WebSocket++ Project nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL PETER THORSON BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 */

#ifndef WEBSOCKETPP_CONNECTION_HPP
#define WEBSOCKETPP_CONNECTION_HPP

#include <websocketpp/close.hpp>
#include <websocketpp/common/cpp11.hpp>
#include <websocketpp/common/functional.hpp>
#include <websocketpp/error.hpp>
#include <websocketpp/processors/processor.hpp>
#include <websocketpp/common/connection_hdl.hpp>

#include <iostream>
#include <vector>
#include <queue>
#include <string>

namespace websocketpp {

typedef lib::function<void(connection_hdl)> open_handler;
typedef lib::function<void(connection_hdl)> close_handler;
typedef lib::function<void(connection_hdl)> fail_handler;

typedef lib::function<void(connection_hdl)> handshake_init_handler;

typedef lib::function<bool(connection_hdl,std::string)> ping_handler;
typedef lib::function<void(connection_hdl,std::string)> pong_handler;
typedef lib::function<void(connection_hdl,std::string)> pong_timeout_handler;


// constants related to the default WebSocket protocol versions available
#ifdef _WEBSOCKETPP_INITIALIZER_LISTS_ // simplified C++11 version
    static const std::vector<int>   VERSIONS_SUPPORTED = {0,7,8,13};
#else
    static const int HELPER[] = {0,7,8,13};
    static const std::vector<int>   VERSIONS_SUPPORTED(HELPER,HELPER+4);
#endif

namespace session {
namespace state {
	// externally visible session state (states based on the RFC)
	
    enum value {
        CONNECTING = 0,
        OPEN = 1,
        CLOSING = 2,
        CLOSED = 3
    };
} // namespace state


namespace fail {
namespace status {
    enum value {
        GOOD = 0,           // no failure yet!
        SYSTEM = 1,         // system call returned error, check that code
        WEBSOCKET = 2,      // websocket close codes contain error
        UNKNOWN = 3,        // No failure information is avaliable
        TIMEOUT_TLS = 4,    // TLS handshake timed out
        TIMEOUT_WS = 5      // WS handshake timed out
    };
} // namespace status
} // namespace fail

namespace internal_state {
	// More granular internal states. These are used for multi-threaded 
	// connection syncronization and preventing values that are not yet or no
	// longer avaliable from being used.
	
	enum value {
		USER_INIT = 0,
		TRANSPORT_INIT = 1,
		READ_HTTP_REQUEST = 2,
		WRITE_HTTP_REQUEST = 3,
		READ_HTTP_RESPONSE = 4,
		WRITE_HTTP_RESPONSE = 5,
		PROCESS_HTTP_REQUEST = 6,
		PROCESS_CONNECTION = 7
	};
} // namespace internal_state
} // namespace session

// impliments the websocket state machine
template <typename config>
class connection
 : public config::transport_type::con_policy
 , public lib::enable_shared_from_this< connection<config> >
{
public:
    typedef connection<config> type;
    typedef lib::shared_ptr<type> ptr;
    typedef lib::weak_ptr<type> weak_ptr;
    
    typedef typename config::concurrency_type concurrency_type;
    typedef typename config::transport_type::con_policy transport_type;
    
    typedef lib::function<void(ptr)> termination_handler;
    
    class handler : public transport_type::handler_interface {
    public:
        typedef handler type;
        typedef lib::shared_ptr<type> ptr;
        typedef lib::weak_ptr<type> weak_ptr;
        
        typedef typename connection::ptr connection_ptr;
        typedef typename config::message_type::ptr message_ptr;
        
        virtual void http(connection_ptr con) {}

        // TODO: validate is server only. hide from client handlers?
        virtual bool validate(connection_ptr con) {return true;} 
        
        virtual void on_inturrupt(connection_ptr con) {}

        virtual void on_open(connection_ptr con) {}
        virtual void on_fail(connection_ptr con) {}
        virtual void on_message(connection_ptr con, message_ptr msg) {}
        virtual void on_close(connection_ptr con) {}

        virtual bool on_ping(connection_ptr con, const std::string &) {
            return true;
        }
        virtual void on_pong(connection_ptr con, const std::string &) {}
        virtual void on_pong_timeout(connection_ptr con, const std::string &) {}

        virtual void on_load(connection_ptr con, ptr old_handler) {}
        virtual void on_unload(connection_ptr con, ptr new_handler) {}
    };
    
    typedef typename handler::ptr handler_ptr;
    
    typedef typename concurrency_type::scoped_lock_type scoped_lock_type;
    typedef typename concurrency_type::mutex_type mutex_type;
    
    typedef typename config::request_type request_type;
    typedef typename config::response_type response_type;
    
	typedef typename config::message_type message_type;
	typedef typename message_type::ptr message_ptr;
	
	typedef typename config::con_msg_manager_type con_msg_manager_type;
    typedef typename con_msg_manager_type::ptr con_msg_manager_ptr;

    typedef processor::processor<config> processor_type;
    typedef lib::shared_ptr<processor_type> processor_ptr;
    
    // Misc Convenience Types
    typedef session::internal_state::value istate_type;

    explicit connection(bool is_server, const std::string& ua)
      : transport_type(is_server)
      , m_user_agent(ua)
      , m_state(session::state::CONNECTING)
      , m_internal_state(session::internal_state::USER_INIT)
      , m_msg_manager(new con_msg_manager_type())
	  , m_send_buffer_size(0)
	  , m_is_server(is_server)
    {
        std::cout << "connection constructor" << std::endl;
    }
    
    // Public Interface
    
    /// Set Connection Handle
    /**
     * The connection handle is a token that can be shared outside the 
     * WebSocket++ core for the purposes of identifying a connection and 
     * sending it messages.
     *
     * @param hdl A connection_hdl that the connection will use to refer
     * to itself.
     */
    void set_handle(connection_hdl hdl) {
        m_connection_hdl = hdl;
    }
    
    /// Get Connection Handle
    /**
     * The connection handle is a token that can be shared outside the
     * WebSocket++ core for the purposes of identifying a connection and
     * sending it messages.
     * 
     * @return A handle to the connection
     */
    connection_hdl get_handle() const {
        return m_connection_hdl;
    }

    void set_open_handler(open_handler h) {
        m_open_handler = h;
    }

    /// Set new connection handler
    /**
     * Will invoke the old handler's on_unload callback followed by the
     * new handler's on_load callback. These callbacks will both happen
     * immediately and before set_handler returns. If called in a method
     * within the connection's strand (such as another callback) the
     * next callback run after the present one will use the new state
     * including ones that are on the asio stack already but not yet 
     * scheduled.
     *
     * This method may be called at any time.
     *
     * @param new_handler The new handler to switch to.
     */
    void set_handler(handler_ptr new_handler);
    
    
    /// Return the same origin policy origin value from the opening request.
    /**
     * This value is available after the HTTP request has been fully read and 
     * may be called from any thread.
     *
     * @return The connection's origin value from the opening handshake.
     */
    const std::string& get_origin() const;
    
	/// Get the size of the outgoing write buffer (in payload bytes)
	/**
	 * Retrieves the number of bytes in the outgoing write buffer that have not
     * already been dispatched to the transport layer. This represents the bytes
     * that are presently cancelable without uncleanly ending the websocket 
     * connection
     *
     * This method invokes the m_write_lock mutex
	 *
	 * @return The current number of bytes in the outgoing send buffer.
	 */
	size_t get_buffered_amount() const;

	/// DEPRECATED: use get_buffered_amount instead
	size_t buffered_amount() const {return get_buffered_amount();}
    
    /// Create a message and then add it to the outgoing send queue
    /**
     * Convenience method to send a message given a payload string and 
     * optionally an opcode. Default opcode is utf8 text.
     *
     * Errors are returned via an exception
     * TODO: make exception system_error rather than error_code
     *
     * This method locks the m_write_lock mutex
     *
     * @param payload The payload string to generated the message with
     *
     * @param op The opcode to generated the message with. Default is 
     * frame::opcode::text
     */
	void send(const std::string& payload, frame::opcode::value op = 
		frame::opcode::TEXT);

    /// Add a message to the outgoing send queue
    /**
     * If presented with a prepared message it is added without validation or 
     * framing. If presented with an unprepared message it is validated, framed,
     * and then added
     *
     * Errors are returned via an exception
     * TODO: make exception system_error rather than error_code
     *
     * This method invokes the m_write_lock mutex
     *
     * @param msg A message_ptr to the message to send.
     */
	void send(message_ptr msg);

    /// Asyncronously invoke handler::on_inturrupt
    /**
     * Signals to the connection to asyncronously invoke the on_inturrupt
     * callback for this connection's handler once it is safe to do so.
     *
     * When the on_inturrupt handler callback is called it will be from
     * within the transport event loop with all the thread safety features
     * guaranteed by the transport to regular handlers
     * 
     * Multiple inturrupt signals can be active at once on the same connection
     *
     * @return An error code
     */
    lib::error_code interrupt();
    
    /// Transport inturrupt callback
    void handle_interrupt();
    
    /// Send a ping
    /**
     * Initiates a ping with the given payload/
     *
     * There is no feedback directly from ping except in cases of immediately
     * detectable errors. Feedback will be provided via on_pong or 
     * on_pong_timeout callbacks.
     *
     * Ping locks the m_write_lock mutex
     *
     * @param payload Payload to be used for the ping
     */
    void ping(const std::string& payload);

    /// Send a pong
    /**
     * Initiates a pong with the given payload.
     *
     * There is no feedback from a pong once sent.
     *
     * Pong locks the m_write_lock mutex
     *
     * @param payload Payload to be used for the pong
     */
    void pong(const std::string & payload);

    /// exception free variant of pong
    void pong(const std::string & payload, lib::error_code & ec);
    
    /// Close the connection
    /**
     * Initiates the close handshake process.
     * 
     * If close returns successfully the connection will be in the closing
     * state and no additional messages may be sent. All messages sent prior
     * to calling close will be written out before the connection is closed.
     *
     * If no reason is specified none will be sent. If no code is specified
     * then no code will be sent.
     *
     * The handler's on_close callback will be called once the close handshake
     * is complete.
     * 
     * Reasons will be automatically truncated to the maximum length (123 bytes)
     * if necessary.
     *
     * @param code The close code to send
     *
     * @param reason The close reason to send
     */
    void close(const close::status::value code, const std::string & reason);
    
    /// exception free variant of close
    void close(const close::status::value code, const std::string & reason, 
        lib::error_code & ec);

    ////////////////////////////////////////////////
    // Pass-through access to the uri information //
    ////////////////////////////////////////////////
    
    /// Returns the secure flag from the connection URI
    /**
     * This value is available after the HTTP request has been fully read and 
     * may be called from any thread.
     *
     * @return Whether or not the connection URI is flagged secure.
     */
    bool get_secure() const;
    
    /// Returns the host component of the connection URI
    /**
     * This value is available after the HTTP request has been fully read and 
     * may be called from any thread.
     *
     * @return The host component of the connection URI
     */
    const std::string& get_host() const;
    
    /// Returns the resource component of the connection URI
    /**
     * This value is available after the HTTP request has been fully read and 
     * may be called from any thread.
     *
     * @return The resource component of the connection URI
     */
    const std::string& get_resource() const;
    
    /// Returns the port component of the connection URI
    /**
     * This value is available after the HTTP request has been fully read and 
     * may be called from any thread.
     *
     * @return The port component of the connection URI
     */
    uint16_t get_port() const;
        
    /////////////////////////////////////////////////////////////
    // Pass-through access to the request and response objects //
    /////////////////////////////////////////////////////////////
    
    /// Set response status code and message
    /**
     * Sets the response status code to `code` and looks up the corresponding
     * message for standard codes. Non-standard codes will be entered as Unknown
     * use set_status(status_code::value,std::string) overload to set both 
     * values explicitly.
     *
     * This member function is valid only from the http() and validate() handler
     * callbacks
     * 
     * @param code Code to set
     * @param msg Message to set
     * @see websocketpp::http::response::set_status
     */
    void set_status(http::status_code::value code);
    
    /// Set response status code and message
    /**
     * Sets the response status code and message to independent custom values.
     * use set_status(status_code::value) to set the code and have the standard
     * message be automatically set.
     *
     * This member function is valid only from the http() and validate() handler
     * callbacks
     * 
     * @param code Code to set
     * @param msg Message to set
     * @see websocketpp::http::response::set_status
     */
    void set_status(http::status_code::value code, const std::string& msg);
    
    /// Set response body content
    /**
     * Set the body content of the HTTP response to the parameter string. Note
     * set_body will also set the Content-Length HTTP header to the appropriate
     * value. If you want the Content-Length header to be something else set it
     * to something else after calling set_body
     *
     * This member function is valid only from the http() and validate() handler
     * callbacks
     * 
     * @param value String data to include as the body content.
     * @see websocketpp::http::response::set_body
     */
    void set_body(const std::string& value);
    
    /// Append a header
    /**
     * If a header with this name already exists the value will be appended to
     * the existing header to form a comma separated list of values. Use
     * replace_header to overwrite existing values.
     *
     * This member function is valid only from the http() and validate() handler
     * callbacks
     * 
     * @param key Name of the header to set
     * @param val Value to add
     * @see replace_header
     * @see websocketpp::http::parser::append_header
     */
    void append_header(const std::string &key,const std::string &val);
    
    /// Replace a header
    /**
     * If a header with this name already exists the old value will be replaced
     * Use add_header to append to a list of existing values.
     *
     * This member function is valid only from the http() and validate() handler
     * callbacks 
     *
     * @param key Name of the header to set
     * @param val Value to set
     * @see add_header
     * @see websocketpp::http::parser::replace_header
     */
    void replace_header(const std::string &key,const std::string &val);
    
    /// Remove a header
    /**
     * Removes a header from the response.
     *
     * This member function is valid only from the http() and validate() handler
     * callbacks
     *
     * @param key The name of the header to remove
     * @see websocketpp::http::parser::remove_header
     */
    void remove_header(const std::string &key);
    
    
    ////////////////////////////////////////////////////////////////////////
    // The remaining public member functions are for internal/policy use  //
    // only. Do not call from application code unless you understand what //
    // you are doing.                                                     //
    ////////////////////////////////////////////////////////////////////////
    
    void start();
    
    void read(size_t num_bytes);
    void handle_read(const lib::error_code& ec, size_t bytes_transferred);
   

    void write(std::string msg);
    void handle_write(const lib::error_code& ec);
    
    void handle_handshake_read(const lib::error_code& ec,
    	size_t bytes_transferred);
    
    void handle_read_frame(const lib::error_code& ec,
        size_t bytes_transferred);
    
    void handle_send_http_response(const lib::error_code& ec);
    
    
    /// Get array of WebSocket protocol versions that this connection supports.
    const std::vector<int>& get_supported_versions() const;
    
    /// Sets the handler for a terminating connection. Should only be used 
    /// internally by the endpoint class.
    void set_termination_handler(termination_handler new_handler);
    
    void terminate();
    
    /// Checks if there are frames in the send queue and if there are sends one
    /**
     * TODO: unit tests
     *
     * This method locks the m_write_lock mutex
     */
    void write_frame();
    
    /// Process the results of a frame write operation and start the next write
    /**
     * TODO: unit tests
     *
     * This method locks the m_write_lock mutex
     *
     * @param terminate Whether or not to terminate the connection upon 
     * completion of this write.
     *
     * @param ec A status code from the transport layer, zero on success, 
     * non-zero otherwise.
     */
    void handle_write_frame(bool terminate, const lib::error_code& ec);
protected:
    void handle_transport_init(const lib::error_code& ec);
    
    /// Set m_processor based on information in m_request. Set m_response 
    /// status and return false on error.
    bool initialize_processor();
    
    /// Perform WebSocket handshake validation of m_request using m_processor. 
    /// set m_response and return false on error.
    bool process_handshake_request();
    
    /// Atomically change the internal connection state.
    /**
     * @param req The required starting state. If the internal state does not 
     * match req an exception is thrown.
     *
     * @param dest The state to change to.
     *
     * @param msg The message to include in the exception thrown
     */
    void atomic_state_change(istate_type req, istate_type dest,
        std::string msg);
    
    /// Atomically change the internal and external connection state.
    /**
     * @param ireq The required starting internal state. If the internal state 
     * does not match ireq an exception is thrown.
     *
     * @param idest The internal state to change to.
     *
     * @param ereq The required starting external state. If the external state 
     * does not match ereq an exception is thrown.
     *
     * @param edest The external state to change to.
     *
     * @param msg The message to include in the exception thrown
     */
    void atomic_state_change(istate_type ireq, istate_type idest, 
        session::state::value ereq, session::state::value edest, 
        std::string msg);
    
    /// Atomically read and compared the internal state.
    /**
     * @param req The state to test against. If the internal state does not 
     * match req an exception is thrown.
     *
     * @param msg The message to include in the exception thrown
     */
    void atomic_state_check(istate_type req, std::string msg);
private:
    /// Completes m_response, serializes it, and sends it out on the wire.
    void send_http_response();
    
    /// Alternate path for send_http_response in error conditions
    void send_http_response_error();

    /// Process control message
    /**
     *
     */
    void process_control_frame(message_ptr msg);
   
    /// Send close acknowledgement
    /**
     * If no arguments are present no close code/reason will be specified.
     *
     * Note: the close code/reason values provided here may be overrided by 
     * other settings (such as silent close).
     *
     * @param code The close code to send
     *
     * @param reason The close reason to send
     *
     * @return A status code, zero on success, non-zero otherwise
     */
    lib::error_code send_close_ack(close::status::value code = 
        close::status::blank, const std::string &reason = "");

    /// Send close frame
    /**
     * If no arguments are present no close code/reason will be specified.
     *
     * Note: the close code/reason values provided here may be overrided by 
     * other settings (such as silent close).
     *
     * The ack flag determines what to do in the case of a blank status and
     * whether or not to terminate the TCP connection after sending it.
     *
     * @param code The close code to send
     *
     * @param reason The close reason to send
     *
     * @param ack Whether or not this is an acknowledgement close frame
     *
     * @return A status code, zero on success, non-zero otherwise
     */
    lib::error_code send_close_frame(close::status::value code = 
        close::status::blank, const std::string &reason = "", bool ack = false,
        bool terminal = false);

    /// Get a pointer to a new WebSocket protocol processor for a given version
    /**
     * @param version Version number of the WebSocket protocol to get a 
     * processor for. Negative values indicate invalid/unknown versions and will
     * always return a null ptr
     * 
     * @return A shared_ptr to a new instance of the appropriate processor or a 
     * null ptr if there is no installed processor that matches the version 
     * number.
     */ 
    processor_ptr get_processor(int version) const;
   
    /// Add a message to the write queue
    /**
     * Adds a message to the write queue and updates any associated shared state
     *
     * Must be called while holding m_write_lock
     *
     * TODO: unit tests
     *
     * @param msg The message to push
     *
     * @return whether or not the queue was empty.
     */
    bool write_push(message_ptr msg);

    /// Pop a message from the write queue
    /**
     * Removes and returns a message from the write queue and updates any 
     * associated shared state.
     *
     * Must be called while holding m_write_lock
     *
     * TODO: unit tests
     *
     * @return the message_ptr at the front of the queue
     */
    message_ptr write_pop();

    // static settings
    const std::string		m_user_agent;
	
    /// Pointer to the handler
    connection_hdl          m_connection_hdl;
    handler_ptr             m_handler;
    open_handler            m_open_handler;

    /// External connection state
    /**
     * Lock: m_connection_state_lock
     */
    session::state::value   m_state;

    /// Internal connection state
    /**
     * Lock: m_connection_state_lock
     */
    istate_type             m_internal_state;

    mutable mutex_type      m_connection_state_lock;

    /// The lock used to protect shared state involved in sending messages
    /**
     * Serializes access to the write queue as well as shared state within the 
     * processor.
     */
    mutex_type              m_write_lock;

    // connection resources
    char                    m_buf[config::connection_read_buffer_size];
    size_t                  m_buf_cursor;
    termination_handler     m_termination_handler;
    con_msg_manager_ptr     m_msg_manager;

    /// Pointer to the processor object for this connection
    /**
     * The processor provides functionality that is specific to the WebSocket 
     * protocol version that the client has negotiated. It also contains all of 
     * the state necessary to encode and decode the incoming and outgoing 
     * WebSocket byte streams
     *
     * Use of the prepare_data_frame method requires lock: m_write_lock
     */
    processor_ptr           m_processor;

    /// Queue of unsent outgoing messages
    /**
     * Lock: m_write_lock
     */
	std::queue<message_ptr> m_send_queue;

    /// Size in bytes of the outstanding payloads in the write queue
    /**
     * Lock: m_write_lock
     */
	size_t m_send_buffer_size;
    
    /// buffer holding the various parts of the current message being writen
    /**
     * Lock m_write_lock
     */
    std::vector<transport::buffer> m_send_buffer;

    // connection data
    request_type            m_request;
    response_type           m_response;
    uri_ptr                 m_uri;
    
    const bool				m_is_server;
    
    // Close state
    /// Close code that was sent on the wire by this endpoint
    close::status::value    m_local_close_code;

    /// Close reason that was sent on the wire by this endpoint
    std::string             m_local_close_reason;

    /// Close code that was received on the wire from the remote endpoint
    close::status::value    m_remote_close_code;

    /// Close reason that was received on the wire from the remote endpoint
    std::string             m_remote_close_reason;
    
    /// Whether or not this endpoint initiated the closing handshake.
    bool                    m_closed_by_me;
    
    /// ???
    bool                    m_failed_by_me;

    /// Whether or not this endpoint initiated the drop of the TCP connection
    bool                    m_dropped_by_me;
};

} // namespace websocketpp

#include <websocketpp/impl/connection_impl.hpp>

#endif // WEBSOCKETPP_CONNECTION_HPP
