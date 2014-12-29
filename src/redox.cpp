/**
* Redis C++11 wrapper.
*/

#include <signal.h>
#include "redox.hpp"

using namespace std;

namespace redox {

void Redox::connected(const redisAsyncContext *ctx, int status) {

  if (status != REDIS_OK) {
    cerr << "[ERROR] Connecting to Redis: " << ctx->errstr << endl;
    return;
  }

  // Disable hiredis automatically freeing reply objects
  ctx->c.reader->fn->freeObject = [](void* reply) {};

  Redox* rdx = (Redox*) ctx->data;
  rdx->connected_lock.unlock();

  cout << "[INFO] Connected to Redis." << endl;
}

void Redox::disconnected(const redisAsyncContext *ctx, int status) {
  if (status != REDIS_OK) {
    cerr << "[ERROR] Disconnecting from Redis: " << ctx->errstr << endl;
    return;
  }

  // Re-enable hiredis automatically freeing reply objects
  ctx->c.reader->fn->freeObject = freeReplyObject;

  Redox* rdx = (Redox*) ctx->data;
  rdx->connected_lock.unlock();

  cout << "[INFO] Disconnected from Redis." << endl;
}

Redox::Redox(const string& host, const int port)
    : host(host), port(port) {

  lock_guard<mutex> lg(queue_guard);

  signal(SIGPIPE, SIG_IGN);

  ctx = redisAsyncConnect(host.c_str(), port);
  if (ctx->err) {
    printf("Error: %s\n", ctx->errstr);
    return;
  }

  evloop = ev_loop_new(EVFLAG_AUTO);
  ev_set_userdata(evloop, (void*)this);

  redisLibevAttach(evloop, ctx);
  redisAsyncSetConnectCallback(ctx, Redox::connected);
  redisAsyncSetDisconnectCallback(ctx, Redox::disconnected);

  ctx->data = (void*)this;
  connected_lock.lock();
}

Redox::~Redox() {

//  cout << "Queue sizes: " << endl;
//  cout << commands_redis_reply.size() << endl;
//  cout << commands_string_r.size() << endl;
//  cout << commands_char_p.size() << endl;
//  cout << commands_int.size() << endl;
//  cout << commands_long_long_int.size() << endl;
//  cout << commands_null.size() << endl;

  redisAsyncDisconnect(ctx);

  stop();

  if(event_loop_thread.joinable())
    event_loop_thread.join();

  ev_loop_destroy(evloop);

  std::cout << "[INFO] Redox created " << commands_created
            << " Commands and freed " << commands_deleted << "." << std::endl;
}

void Redox::run_blocking() {

  // Events to connect to Redox
  ev_run(evloop, EVRUN_NOWAIT);

  // Block until connected to Redis
  connected_lock.lock();
  connected_lock.unlock();

  // Continuously create events and handle them
  while (!to_exit) {
    process_queued_commands();
    ev_run(evloop, EVRUN_NOWAIT);
  }

  cout << "[INFO] Stop signal detected." << endl;

  // Run a few more times to clear out canceled events
//  for(int i = 0; i < 100; i++) {
//    ev_run(evloop, EVRUN_NOWAIT);
//  }

  // Run until all commands are processed
  do {
    ev_run(evloop, EVRUN_NOWAIT);
  } while(commands_created != commands_deleted);

  exited = true;

  // Let go for block_until_stopped method
  exit_waiter.notify_one();

  cout << "[INFO] Event thread exited." << endl;
}

void Redox::run() {

  event_loop_thread = thread([this] { run_blocking(); });

  // Don't return until connected
  lock_guard<mutex> lg(connected_lock);
}

void Redox::stop_signal() {
  to_exit = true;
}

void Redox::block() {
  unique_lock<mutex> ul(exit_waiter_lock);
  exit_waiter.wait(ul, [this] { return exited.load(); });
}

void Redox::stop() {
  stop_signal();
  block();
}

template<class ReplyT>
void command_callback(redisAsyncContext *ctx, void *r, void *privdata) {

  Redox* rdx = (Redox*) ctx->data;
  long id = (long)privdata;
  redisReply* reply_obj = (redisReply*) r;

  auto& command_map = rdx->get_command_map<ReplyT>();
  auto it = command_map.find(id);
  if(it == command_map.end()) {
    cout << "[ERROR] Couldn't find Command " << id << " in command_map." << endl;
    freeReplyObject(r);
    return;
  };
  Command<ReplyT>* c = it->second;

  if(!rdx->is_active_command(c->id)) {
    std::cout << "[INFO] Ignoring callback, command " << c << " was freed." << std::endl;
    freeReplyObject(r);
    return;
  }

  if(c->cmd == "GET simple_loop:count") {
    std::cout << "In command_callback = " << c->cmd << " at " << c << ", reply_obj = " << reply_obj << std::endl;
    std::cout << "reply type: " << reply_obj->type << std::endl;
    std::cout << "reply int: " << reply_obj->integer << std::endl;
    std::cout << "reply str: " << reply_obj->str << std::endl;
//    std::string s(reply_obj->str);
//    std::cout << "string object: " << s << std::endl;
  }

  c->reply_obj = reply_obj;
  c->process_reply();

  // Increment the Redox object command counter
  rdx->incr_cmd_count();
}

/**
* Submit an asynchronous command to the Redox server. Return
* true if succeeded, false otherwise.
*/
template<class ReplyT>
bool submit_to_server(Command<ReplyT>* c) {
  if(c->cmd == "GET simple_loop:count") {
    std::cout << "submit_to_server for cmd at " << c << ": " << c->cmd << std::endl;
  }
  c->pending++;
  if (redisAsyncCommand(c->rdx->ctx, command_callback<ReplyT>, (void*)c->id, c->cmd.c_str()) != REDIS_OK) {
    cerr << "[ERROR] Could not send \"" << c->cmd << "\": " << c->rdx->ctx->errstr << endl;
    c->invoke_error(REDOX_SEND_ERROR);
    return false;
  }
  return true;
}

template<class ReplyT>
void submit_command_callback(struct ev_loop* loop, ev_timer* timer, int revents) {

  Redox* rdx = (Redox*) ev_userdata(loop);
  long id = (long)timer->data;

  auto& command_map = rdx->get_command_map<ReplyT>();
  auto it = command_map.find(id);
  if(it == command_map.end()) {
    cout << "[ERROR] Couldn't find Command " << id << " in command_map." << endl;
    return;
  };
  Command<ReplyT>* c = it->second;

  if(c->is_completed()) {

    cerr << "[INFO] Command " << c << " is completed, stopping event timer." << endl;

    c->timer_guard.lock();
    if((c->repeat != 0) || (c->after != 0))
      ev_timer_stop(loop, &c->timer);
    c->timer_guard.unlock();

    Command<ReplyT>::free_command(c);

    return;
  }

  submit_to_server<ReplyT>(c);
}

template<class ReplyT>
bool Redox::process_queued_command(long id) {

  auto& command_map = get_command_map<ReplyT>();

  auto it = command_map.find(id);
  if(it == command_map.end()) return false;
  Command<ReplyT>* c = it->second;

  if(c->cmd == "GET simple_loop:count") {
    std::cout << "process_queued_command for cmd at " << c << ": " << c->cmd << std::endl;
  }

  if((c->repeat == 0) && (c->after == 0)) {
    submit_to_server<ReplyT>(c);
  } else {

    c->timer.data = (void*)c->id;

    ev_timer_init(&c->timer, submit_command_callback<ReplyT>, c->after, c->repeat);
    ev_timer_start(evloop, &c->timer);

    c->timer_guard.unlock();
  }

  return true;
}

void Redox::process_queued_commands() {

  lock_guard<mutex> lg(queue_guard);

  while(!command_queue.empty()) {

    long id = command_queue.front();
    if(process_queued_command<redisReply*>(id)) {}
    else if(process_queued_command<string>(id)) {}
    else if(process_queued_command<char*>(id)) {}
    else if(process_queued_command<int>(id)) {}
    else if(process_queued_command<long long int>(id)) {}
    else if(process_queued_command<nullptr_t>(id)) {}
    else throw runtime_error("[FATAL] Command pointer not found in any queue!");

    command_queue.pop();
  }
}

// ----------------------------

template<> unordered_map<long, Command<redisReply*>*>&
Redox::get_command_map() { return commands_redis_reply; }

template<> unordered_map<long, Command<string>*>&
Redox::get_command_map() { return commands_string_r; }

template<> unordered_map<long, Command<char*>*>&
Redox::get_command_map() { return commands_char_p; }

template<> unordered_map<long, Command<int>*>&
Redox::get_command_map() { return commands_int; }

template<> unordered_map<long, Command<long long int>*>&
Redox::get_command_map() { return commands_long_long_int; }

template<> unordered_map<long, Command<nullptr_t>*>&
Redox::get_command_map() { return commands_null; }

// ----------------------------
// Helpers
// ----------------------------

void Redox::command(const string& cmd) {
  command<redisReply*>(cmd);
}

bool Redox::command_blocking(const string& cmd) {
  Command<redisReply*>* c = command_blocking<redisReply*>(cmd);
  bool succeeded = (c->status() == REDOX_OK);
  c->free();
  return succeeded;
}

} // End namespace redis
