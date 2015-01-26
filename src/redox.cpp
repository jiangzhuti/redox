/**
* Redis C++11 wrapper.
*/

#include <signal.h>
#include "redox.hpp"
#include <string.h>

using namespace std;

namespace redox {

void Redox::connected_callback(const redisAsyncContext *ctx, int status) {

  Redox* rdx = (Redox*) ctx->data;

  if (status != REDIS_OK) {
    rdx->logger.fatal() << "Could not connect to Redis: " << ctx->errstr;
    rdx->connect_state = REDOX_CONNECT_ERROR;

  } else {
    // Disable hiredis automatically freeing reply objects
    ctx->c.reader->fn->freeObject = [](void *reply) {};
    rdx->connect_state = REDOX_CONNECTED;
    rdx->logger.info() << "Connected to Redis.";
  }

  rdx->connect_waiter.notify_all();
  if(rdx->user_connection_callback) rdx->user_connection_callback(rdx->connect_state);
}

void Redox::disconnected_callback(const redisAsyncContext *ctx, int status) {

  Redox* rdx = (Redox*) ctx->data;

  if (status != REDIS_OK) {
    rdx->logger.error() << "Could not disconnect from Redis: " << ctx->errstr;
    rdx->connect_state = REDOX_DISCONNECT_ERROR;
  } else {
    rdx->logger.info() << "Disconnected from Redis as planned.";
    rdx->connect_state = REDOX_DISCONNECTED;
  }

  rdx->stop_signal();
  rdx->connect_waiter.notify_all();
  if(rdx->user_connection_callback) rdx->user_connection_callback(rdx->connect_state);
}

void Redox::init_ev() {
  signal(SIGPIPE, SIG_IGN);
  evloop = ev_loop_new(EVFLAG_AUTO);
  ev_set_userdata(evloop, (void*)this); // Back-reference
}

void Redox::init_hiredis() {

  ctx->data = (void*)this; // Back-reference

  if (ctx->err) {
    logger.error() << "Could not create a hiredis context: " << ctx->errstr;
    connect_state = REDOX_CONNECT_ERROR;
    connect_waiter.notify_all();
    return;
  }

  // Attach event loop to hiredis
  redisLibevAttach(evloop, ctx);

  // Set the callbacks to be invoked on server connection/disconnection
  redisAsyncSetConnectCallback(ctx, Redox::connected_callback);
  redisAsyncSetDisconnectCallback(ctx, Redox::disconnected_callback);
}

Redox::Redox(
  const string& host, const int port,
  function<void(int)> connection_callback,
  ostream& log_stream,
  log::Level log_level
) : host(host), port(port),
    logger(log_stream, log_level),
    user_connection_callback(connection_callback) {

  init_ev();

  // Connect over TCP
  ctx = redisAsyncConnect(host.c_str(), port);

  init_hiredis();
}

Redox::Redox(
  const string& path,
  function<void(int)> connection_callback,
  ostream& log_stream,
  log::Level log_level
) : host(), port(), path(path), logger(log_stream, log_level),
    user_connection_callback(connection_callback) {

  init_ev();

  // Connect over unix sockets
  ctx = redisAsyncConnectUnix(path.c_str());

  init_hiredis();
}

void break_event_loop(struct ev_loop* loop, ev_async* async, int revents) {
  ev_break(loop, EVBREAK_ALL);
}

void Redox::run_event_loop() {

  // Events to connect to Redox
  ev_run(evloop, EVRUN_NOWAIT);

  // Block until connected to Redis, or error
  unique_lock<mutex> ul(connect_lock);
  connect_waiter.wait(ul, [this] { return connect_state != REDOX_NOT_YET_CONNECTED; });

  // Handle connection error
  if(connect_state != REDOX_CONNECTED) {
    logger.warning() << "Did not connect, event loop exiting.";
    running_waiter.notify_one();
    return;
  }

  // Set up asynchronous watcher which we signal every
  // time we add a command
  ev_async_init(&async_w, process_queued_commands);
  ev_async_start(evloop, &async_w);

  // Set up an async watcher to break the loop
  ev_async_init(&async_stop, break_event_loop);
  ev_async_start(evloop, &async_stop);

  running = true;
  running_waiter.notify_one();

  // Run the event loop
  while (!to_exit) {
//    logger.info() << "Event loop running";
    ev_run(evloop, EVRUN_NOWAIT);
  }

  logger.info() << "Stop signal detected.";

  // Run a few more times to clear out canceled events
  for(int i = 0; i < 100; i++) {
    ev_run(evloop, EVRUN_NOWAIT);
  }

  if(commands_created != commands_deleted) {
    logger.error() << "All commands were not freed! "
         << commands_deleted << "/" << commands_created;
  }

  exited = true;
  running = false;

  // Let go for block_until_stopped method
  exit_waiter.notify_one();

  logger.info() << "Event thread exited.";
}

bool Redox::start() {

  event_loop_thread = thread([this] { run_event_loop(); });

  // Block until connected and running the event loop, or until
  // a connection error happens and the event loop exits
  unique_lock<mutex> ul(running_waiter_lock);
  running_waiter.wait(ul, [this] {
    return running.load() || connect_state == REDOX_CONNECT_ERROR;
  });

  // Return if succeeded
  return connect_state == REDOX_CONNECTED;
}

void Redox::stop_signal() {
  to_exit = true;
  logger.debug() << "stop_signal() called, breaking event loop";
  ev_async_send(evloop, &async_stop);
}

void Redox::block() {
  unique_lock<mutex> ul(exit_waiter_lock);
  exit_waiter.wait(ul, [this] { return exited.load(); });
}

void Redox::stop() {
  stop_signal();
  block();
}

void Redox::disconnect() {
  stop_signal();
  if(connect_state == REDOX_CONNECTED) {
    redisAsyncDisconnect(ctx);
    block();
  }
}

Redox::~Redox() {

  disconnect();

  if(event_loop_thread.joinable())
    event_loop_thread.join();

  ev_loop_destroy(evloop);

  logger.info() << "Redox created " << commands_created
    << " Commands and freed " << commands_deleted << ".";
}

template<class ReplyT>
Command<ReplyT>* Redox::find_command(long id) {

  lock_guard<mutex> lg(command_map_guard);

  auto& command_map = get_command_map<ReplyT>();
  auto it = command_map.find(id);
  if(it == command_map.end()) return nullptr;
  return it->second;
}

template<class ReplyT>
void Redox::command_callback(redisAsyncContext *ctx, void *r, void *privdata) {

  Redox* rdx = (Redox*) ctx->data;
  long id = (long)privdata;
  redisReply* reply_obj = (redisReply*) r;

  Command<ReplyT>* c = rdx->find_command<ReplyT>(id);
  if(c == nullptr) {
//    rdx->logger.warning() << "Couldn't find Command " << id << " in command_map (command_callback).";
    freeReplyObject(reply_obj);
    return;
  }

  c->processReply(reply_obj);

  // Increment the Redox object command counter
  rdx->cmd_count++;
}

/**
* Submit an asynchronous command to the Redox server. Return
* true if succeeded, false otherwise.
*/
template<class ReplyT>
bool Redox::submit_to_server(Command<ReplyT>* c) {

  Redox* rdx = c->rdx_;
  c->pending_++;

  // Process binary data if trailing quotation. This is a limited implementation
  // to allow binary data between the first and the last quotes of the command string,
  // if the very last character of the command is a quote ('"').
  if(c->cmd_[c->cmd_.size()-1] == '"') {

    // Indices of the quotes
    size_t first = c->cmd_.find('"');
    size_t last = c->cmd_.size()-1;

    // Proceed only if the first and last quotes are different
    if(first != last) {

      string format = c->cmd_.substr(0, first) + "%b";
      string value = c->cmd_.substr(first+1, last-first-1);
      if (redisAsyncCommand(rdx->ctx, command_callback<ReplyT>, (void*)c->id_, format.c_str(), value.c_str(), value.size()) != REDIS_OK) {
        rdx->logger.error() << "Could not send \"" << c->cmd_ << "\": " << rdx->ctx->errstr;
        c->reply_status_ = Command<ReplyT>::SEND_ERROR;
        c->invoke();
        return false;
      }
      return true;
    }
  }

  if (redisAsyncCommand(rdx->ctx, command_callback<ReplyT>, (void*)c->id_, c->cmd_.c_str()) != REDIS_OK) {
    rdx->logger.error() << "Could not send \"" << c->cmd_ << "\": " << rdx->ctx->errstr;
    c->reply_status_ = Command<ReplyT>::SEND_ERROR;
    c->invoke();
    return false;
  }

  return true;
}

template<class ReplyT>
void Redox::submit_command_callback(struct ev_loop* loop, ev_timer* timer, int revents) {

  Redox* rdx = (Redox*) ev_userdata(loop);
  long id = (long)timer->data;

  Command<ReplyT>* c = rdx->find_command<ReplyT>(id);
  if(c == nullptr) {
    rdx->logger.error() << "Couldn't find Command " << id
         << " in command_map (submit_command_callback).";
    return;
  }

  if(c->canceled()) {

//    logger.info() << "Command " << c << " is completed, stopping event timer.";

    c->timer_guard_.lock();
    if((c->repeat_ != 0) || (c->after_ != 0))
      ev_timer_stop(loop, &c->timer_);
    c->timer_guard_.unlock();

    // Mark for memory to be freed when all callbacks are received
    c->timer_.data = (void*)(long)0;

    return;
  }

  submit_to_server<ReplyT>(c);
}

template<class ReplyT>
bool Redox::process_queued_command(long id) {

  Command<ReplyT>* c = find_command<ReplyT>(id);
  if(c == nullptr) return false;

  if((c->repeat_ == 0) && (c->after_ == 0)) {
    submit_to_server<ReplyT>(c);

  } else {

    c->timer_.data = (void*)c->id_;
    ev_timer_init(&c->timer_, submit_command_callback<ReplyT>, c->after_, c->repeat_);
    ev_timer_start(evloop, &c->timer_);

    c->timer_guard_.unlock();
  }

  return true;
}

void Redox::process_queued_commands(struct ev_loop* loop, ev_async* async, int revents) {

  Redox* rdx = (Redox*) ev_userdata(loop);

  lock_guard<mutex> lg(rdx->queue_guard);

  while(!rdx->command_queue.empty()) {

    long id = rdx->command_queue.front();
    rdx->command_queue.pop();

    if(rdx->process_queued_command<redisReply*>(id)) {}
    else if(rdx->process_queued_command<string>(id)) {}
    else if(rdx->process_queued_command<char*>(id)) {}
    else if(rdx->process_queued_command<int>(id)) {}
    else if(rdx->process_queued_command<long long int>(id)) {}
    else if(rdx->process_queued_command<nullptr_t>(id)) {}
    else if(rdx->process_queued_command<vector<string>>(id)) {}
    else if(rdx->process_queued_command<std::set<string>>(id)) {}
    else if(rdx->process_queued_command<unordered_set<string>>(id)) {}
    else throw runtime_error("Command pointer not found in any queue!");
  }
}

// ---------------------------------
// Pub/Sub methods
// ---------------------------------

void Redox::subscribe_raw(const string cmd_name, const string topic,
  function<void(const string&, const string&)> msg_callback,
  function<void(const string&)> sub_callback,
  function<void(const string&)> unsub_callback,
  function<void(const string&, int)> err_callback
) {

  // Start pubsub mode. No non-sub/unsub commands can be emitted by this client.
  pubsub_mode = true;

  command_looping<redisReply*>(cmd_name + " " + topic,
    [this, topic, msg_callback, err_callback, sub_callback, unsub_callback](Command<redisReply*>& c) {

      if(!c.ok()) {
        if(err_callback) err_callback(topic, c.status());
        return;
      }

      redisReply* reply = c.reply();

      // For debugging only
//      cout << "------" << endl;
//      cout << cmd << " " << (reply->type == REDIS_REPLY_ARRAY) << " " << (reply->elements) << endl;
//      for(int i = 0; i < reply->elements; i++) {
//        redisReply* r = reply->element[i];
//        cout << "element " << i << ", reply type = " << r->type << " ";
//        if(r->type == REDIS_REPLY_STRING) cout << r->str << endl;
//        else if(r->type == REDIS_REPLY_INTEGER) cout << r->integer << endl;
//        else cout << "some other type" << endl;
//      }
//      cout << "------" << endl;

      // TODO cancel this command on unsubscription?

      // If the last entry is an integer, then it is a [p]sub/[p]unsub command
      if((reply->type == REDIS_REPLY_ARRAY) &&
        (reply->element[reply->elements-1]->type == REDIS_REPLY_INTEGER)) {

        if(!strncmp(reply->element[0]->str, "sub", 3)) {
          subscribed_topics_.insert(topic);
          if(sub_callback) sub_callback(topic);

        } else if(!strncmp(reply->element[0]->str, "psub", 4)) {
          psubscribed_topics_.insert(topic);
          if (sub_callback) sub_callback(topic);

        } else if(!strncmp(reply->element[0]->str, "uns", 3)) {
          subscribed_topics_.erase(topic);
          if (unsub_callback) unsub_callback(topic);

        } else if(!strncmp(reply->element[0]->str, "puns", 4)) {
          psubscribed_topics_.erase(topic);
          if (unsub_callback) unsub_callback(topic);
        }

        else logger.error() << "Unknown pubsub message: " << reply->element[0]->str;
      }

      // Message for subscribe
      else if ((reply->type == REDIS_REPLY_ARRAY) && (reply->elements == 3)) {
        char *msg = reply->element[2]->str;
        if (msg && msg_callback) msg_callback(topic, reply->element[2]->str);
      }

      // Message for psubscribe
      else if ((reply->type == REDIS_REPLY_ARRAY) && (reply->elements == 4)) {
        char *msg = reply->element[2]->str;
        if (msg && msg_callback) msg_callback(reply->element[2]->str, reply->element[3]->str);
      }

      else logger.error() << "Unknown pubsub message of type " << reply->type;
    },
    1e10 // To keep the command around for a few hundred years
  );
}

void Redox::subscribe(const string topic,
  function<void(const string&, const string&)> msg_callback,
  function<void(const string&)> sub_callback,
  function<void(const string&)> unsub_callback,
  function<void(const string&, int)> err_callback
) {
  if(subscribed_topics_.find(topic) != subscribed_topics_.end()) {
    logger.warning() << "Already subscribed to " << topic << "!";
    return;
  }
  subscribe_raw("SUBSCRIBE", topic, msg_callback, sub_callback, unsub_callback, err_callback);
}

void Redox::psubscribe(const string topic,
  function<void(const string&, const string&)> msg_callback,
  function<void(const string&)> sub_callback,
  function<void(const string&)> unsub_callback,
  function<void(const string&, int)> err_callback
) {
  if(psubscribed_topics_.find(topic) != psubscribed_topics_.end()) {
    logger.warning() << "Already psubscribed to " << topic << "!";
    return;
  }
  subscribe_raw("PSUBSCRIBE", topic, msg_callback, sub_callback, unsub_callback, err_callback);
}

void Redox::unsubscribe_raw(const string cmd_name, const string topic,
  function<void(const string&, int)> err_callback
) {
  command<redisReply*>(cmd_name + " " + topic,
    [topic, err_callback](Command<redisReply*>& c) {
      if(!c.ok()) {
        if (err_callback) err_callback(topic, c.status());
      }
    }
  );
}

void Redox::unsubscribe(const string topic,
  function<void(const string&, int)> err_callback
) {
  if(subscribed_topics_.find(topic) == subscribed_topics_.end()) {
    logger.warning() << "Cannot unsubscribe from " << topic << ", not subscribed!";
    return;
  }
  unsubscribe_raw("UNSUBSCRIBE", topic, err_callback);
}

void Redox::punsubscribe(const string topic,
  function<void(const string&, int)> err_callback
) {
  if(psubscribed_topics_.find(topic) == psubscribed_topics_.end()) {
    logger.warning() << "Cannot punsubscribe from " << topic << ", not psubscribed!";
    return;
  }
  unsubscribe_raw("PUNSUBSCRIBE", topic, err_callback);
}

void Redox::publish(const string topic, const string msg,
  function<void(const string&, const string&)> pub_callback,
  function<void(const string&, int)> err_callback
) {
  command<redisReply*>("PUBLISH " + topic + " " + msg,
    [topic, msg, err_callback, pub_callback](Command<redisReply*>& c) {
      if(!c.ok()) {
        if(err_callback) err_callback(topic, c.status());
      }
      if(pub_callback) pub_callback(topic, msg);
    }
  );
}

/**
* Throw an exception for any non-pubsub commands.
*/
void Redox::deny_non_pubsub(const string& cmd) {

  string cmd_name = cmd.substr(0, cmd.find(' '));

  // Compare with the command's first 5 characters
  if(!cmd_name.compare("SUBSCRIBE") || !cmd_name.compare("UNSUBSCRIBE") ||
    !cmd_name.compare("PSUBSCRIBE") || !cmd_name.compare("PUNSUBSCRIBE")) {
  } else {
    throw runtime_error("In pub/sub mode, this Redox instance can only issue "
      "[p]subscribe/[p]unsubscribe commands! Use another instance for other commands.");
  }
}

// ---------------------------------
// get_command_map specializations
// ---------------------------------

template<> unordered_map<long, Command<redisReply*>*>&
Redox::get_command_map<redisReply*>() { return commands_redis_reply; }

template<> unordered_map<long, Command<string>*>&
Redox::get_command_map<string>() { return commands_string_r; }

template<> unordered_map<long, Command<char*>*>&
Redox::get_command_map<char*>() { return commands_char_p; }

template<> unordered_map<long, Command<int>*>&
Redox::get_command_map<int>() { return commands_int; }

template<> unordered_map<long, Command<long long int>*>&
Redox::get_command_map<long long int>() { return commands_long_long_int; }

template<> unordered_map<long, Command<nullptr_t>*>&
Redox::get_command_map<nullptr_t>() { return commands_null; }

template<> unordered_map<long, Command<vector<string>>*>&
Redox::get_command_map<vector<string>>() { return commands_vector_string; }

template<> unordered_map<long, Command<set<string>>*>&
Redox::get_command_map<set<string>>() { return commands_set_string; }

template<> unordered_map<long, Command<unordered_set<string>>*>&
Redox::get_command_map<unordered_set<string>>() { return commands_unordered_set_string; }

// ----------------------------
// Helpers
// ----------------------------

bool Redox::command_blocking(const string& cmd) {
  auto& c = command_blocking<redisReply*>(cmd);
  bool succeeded = c.ok();
  c.free();
  return succeeded;
}

string Redox::get(const string& key) {

  Command<char*>& c = command_blocking<char*>("GET " + key);
  if(!c.ok()) {
    throw runtime_error("[FATAL] Error getting key " + key + ": Status code " + to_string(c.status()));
  }
  string reply = c.reply();
  c.free();
  return reply;
};

bool Redox::set(const string& key, const string& value) {
  return command_blocking("SET " + key + " " + value);
}

bool Redox::del(const string& key) {
  return command_blocking("DEL " + key);
}

} // End namespace redis
