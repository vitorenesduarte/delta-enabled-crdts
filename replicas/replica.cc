// 
// Implementation (not ready) of the anti-entropy algorithm described in
// http://haslab.uminho.pt/ashoker/files/deltacrdt.pdf
// 
#include <unistd.h> // sleep
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <time.h>
#include <utility> // pair
#include "../delta-crdts.cc"
#include "../csock/csocket.h"
#include "../csock/csocketserver.h"
#include "../helpers.h"
#include "../message.pb.h"

using namespace std;
bool REPL = false;
bool DELTA = true;
bool OPTIMIZATION = true;

void id_and_port(string& s, int& id, int& port);
void id_host_and_port(string& s, int& id, string& host, int& port);

void show_usage();
void show_crdt(twopset<string>& crdt);

time_t now();
void log_message_sent();
void log_bytes_received(proto::message& message);
void log_new_state(twopset<string>& crdt);
void log_op(string& op);


void socket_reader(int my_id, int& seq, twopset<string>& crdt, map<int, pair<int, twopset<string>>>& seq_to_delta, map<int, int>& id_to_ack, csocketserver& socket_server, mutex& mtx)
{
  while(true)
  {
    map<int, proto::message> fd_to_new_messages;
    socket_server.act(fd_to_new_messages);

    if(!fd_to_new_messages.empty())
    {
      for(const auto& kv : fd_to_new_messages)
      {
        proto::message message = kv.second;
        log_bytes_received(message);

        if(message.type() == proto::message::TWOPSET)
        {
          // 9 on receive(delta, d, n)
          twopset<string> delta;
          message >> delta;
          int replica_id = message.id();
          int message_seq = message.seq();

          mtx.lock();
          if(!(delta <= crdt))
          {
            // 10 if !(d <= Xi)
            crdt.join(delta);
            // NEW
            seq_to_delta[seq++] = make_pair(replica_id, delta);

            log_new_state(crdt);
            show_crdt(crdt);
          }
          mtx.unlock();

          proto::message ack;
          ack.set_type(proto::message::ACK);
          ack.set_id(my_id);
          ack.set_seq(message_seq);

          int replica_fd = socket_server.id_to_fd()[replica_id];
          helper::pb::send(replica_fd, ack);
        }
        else if(message.type() == proto::message::ID)
        {
          mtx.lock();
          socket_server.set_id(kv.first, message.id());
          mtx.unlock();
        }
        else if(message.type() == proto::message::ACK)
        {
          // 15 one receive (ack, n)
          mtx.lock();
          int replica_id = message.id();
          int new_ack = message.seq();
          int current_ack = id_to_ack.count(replica_id) ? id_to_ack[replica_id] : 0;
          int max = current_ack > new_ack ? current_ack : new_ack;

          id_to_ack[replica_id] = max;
          mtx.unlock();
        } else cout << "Can't handle messages with type " << message.type() << endl;
      }
    }
  }
}

void keyboard_reader(int my_id, int& seq, twopset<string>& crdt, map<int, pair<int, twopset<string>>>& seq_to_delta, map<int, int>& id_to_ack, csocketserver& socket_server, mutex& mtx)
{
  show_usage();

  string line;
  while(getline(cin, line))
  {
    vector<string> parts = helper::str::split(line, ' ');
    if(!parts.empty())
    {
      log_op(line);
      if(parts.front() == "add" || parts.front() == "rmv")
      {
        // 17 on operation
        twopset<string> delta;

        mtx.lock();
        for(int i = 1; i < parts.size(); i++)
        {
          if(parts.front() == "add") delta.join(crdt.add(parts.at(i)));
          else delta.join(crdt.rmv(parts.at(i)));
        }

        show_crdt(crdt);

        // NEW
        seq_to_delta[seq++] = make_pair(my_id, delta);
        mtx.unlock();
      } 
      else if(parts.front() == "show") cout << crdt << endl;
      else if(parts.front() == "connect")
      {
        for(int i = 1; i < parts.size(); i++)
        {
          int replica_id, replica_port;
          string host;
          id_host_and_port(parts.at(i), replica_id, host, replica_port);

          char* host_ = new char[host.length() + 1];
          strcpy(host_, host.c_str());

          int replica_fd = helper::net::connect_to(host_, replica_port);

          mtx.lock();
          socket_server.add_fd(replica_fd);
          socket_server.set_id(replica_fd, replica_id);
          mtx.unlock();

          // tell that replica my id
          proto::message id_message;
          id_message.set_type(proto::message::ID);
          id_message.set_id(my_id);
          id_message.set_seq(0);
          helper::pb::send(replica_fd, id_message);
        }
      } 
      else if(parts.front() == "wait" && parts.size() > 1)
      {
        int seconds = atoi(parts.at(1).c_str());
        sleep(seconds);
      }
      else cout << "Unrecognized option" << endl;
    }
  }
}

void garbage_collect_deltas(map<int, pair<int, twopset<string>>>& seq_to_delta, map<int, int>& id_to_ack, mutex& mtx)
{
  if(seq_to_delta.empty()) return; // if nothing to collect

  map<int, pair<int, twopset<string>>> new_seq_to_delta;
  mtx.lock();

  vector<int> acks = helper::map::values(id_to_ack);
  int min = helper::min(acks);

  for(const auto& kv : seq_to_delta)
    if(kv.first >= min)
      new_seq_to_delta.emplace(kv.first, kv.second);

  seq_to_delta = new_seq_to_delta;
  mtx.unlock();
}

void gossiper(int my_id, int& seq, twopset<string>& crdt, map<int, pair<int, twopset<string>>>& seq_to_delta, map<int, int>& id_to_ack, csocketserver& socket_server, mutex& mtx, int& gossip_sleep_time, int& fanout)
{
  sleep(gossip_sleep_time);

  // 30 garbage collect deltas
  garbage_collect_deltas(seq_to_delta, id_to_ack, mtx);

  // 22 periodically ship delta-interval or state
  int this_seq;
  map<int, twopset<string>> fd_to_delta;

  mtx.lock();
  map<int, int> id_to_fd = socket_server.id_to_fd();
  if(!id_to_fd.empty())
  {
    set<int> ids = helper::map::keys(id_to_fd);
    if(fanout != -1) ids = helper::random(ids, fanout);

    for(auto& replica_id : ids)
    {
      twopset<string> delta;
      int replica_fd = id_to_fd[replica_id];

      int last_ack = id_to_ack.count(replica_id) ? id_to_ack[replica_id] : 0;
      set<int> seqs = helper::map::keys(seq_to_delta);
      int min = helper::min(seqs);

      // 24
      bool whole_state = seq_to_delta.empty() || min > last_ack;

      // To support sending the whole state
      if(!DELTA) whole_state = true;

      if(whole_state) delta = crdt;
      else
      {
        for(int i = last_ack; i < seq; i++)
        {
          // NEW
          if(OPTIMIZATION)
          {
            int from = seq_to_delta[i].first;
            if(from != replica_id) delta.join(seq_to_delta[i].second);
          }
          else delta.join(seq_to_delta[i].second);
        }
      }

      // 28
      // NEW
      bool should_gossip = last_ack < seq && delta.read().size() > 0;
      if(should_gossip) fd_to_delta[replica_fd] = delta;
    }

    this_seq = seq;
  }
  mtx.unlock();

  for(auto& kv : fd_to_delta)
  {
    int replica_fd = kv.first;
    proto::message message;
    message << kv.second;
    message.set_id(my_id);
    message.set_seq(this_seq);
    helper::pb::send(replica_fd, message);
    log_message_sent();
  }

  gossiper(my_id, seq, crdt, seq_to_delta, id_to_ack, socket_server, mtx, gossip_sleep_time, fanout);
}

int main(int argc, char *argv[])
{
  if(argc < 2)
  {
    cerr << "Usage: " << argv[0] << " unique_id:port [-r] [-t gossip_sleep_time] [-f fanout] [-d] [-s]" << endl;
    exit(0);
  } 

  int gossip_sleep_time = 10;
  int fanout = 1;

  int id, port;
  string arg(argv[1]);
  id_and_port(arg, id, port);

  for(int i = 2; i < argc; i++)
  {
    string arg(argv[i]);
    if(arg == "-r") REPL = true;
    else if(arg == "-t") gossip_sleep_time = atoi(argv[++i]);
    else if(arg == "-f") fanout = atoi(argv[++i]);
    else if(arg == "-d") DELTA = true;
    else if(arg == "-s") DELTA = false;
    else if(arg == "-no") OPTIMIZATION = false;
    // TODO deal with bad usage
  }

  int socket_server_fd = helper::net::listen_on(port);
  csocketserver socket_server(socket_server_fd);

  mutex mtx;

  // 3 durable state:
  int seq = 0;
  twopset<string> crdt;
  // 6 volatile state:
  map<int, pair<int, twopset<string>>> seq_to_delta;
  map<int, int> id_to_ack;

  thread sr(
      socket_reader,
      id,
      ref(seq),
      ref(crdt),
      ref(seq_to_delta),
      ref(id_to_ack),
      ref(socket_server),
      ref(mtx)
  );

  thread g(
      gossiper,
      id,
      ref(seq),
      ref(crdt),
      ref(seq_to_delta),
      ref(id_to_ack),
      ref(socket_server),
      ref(mtx),
      ref(gossip_sleep_time),
      ref(fanout)
  );

  keyboard_reader(
      id,
      ref(seq),
      ref(crdt),
      ref(seq_to_delta),
      ref(id_to_ack),
      ref(socket_server),
      ref(mtx)
  );

  sr.join();
  g.join();

  return 0;
}

void id_and_port(string& s, int& id, int& port)
{
  vector<string> v = helper::str::split(s, ':');
  id = atoi(v.at(0).c_str());
  port = atoi(v.at(1).c_str());
}

void id_host_and_port(string& s, int& id, string& host, int& port)
{
  vector<string> v = helper::str::split(s, ':');

  id = atoi(v.at(0).c_str());
  host = v.at(1);
  port = atoi(v.at(2).c_str());
}

mutex stdout_mtx;
void l() { stdout_mtx.lock(); }
void ul() {stdout_mtx.unlock(); }

void show_usage()
{
  if(!REPL) return;
  l();
  cout << "Usage:\n";
  cout << "add [elems]\n";
  cout << "rmv [elems]\n";
  cout << "show\n";
  cout << "connect [unique_id:host:port]\n";
  cout << "wait seconds" << endl;
  ul();
}

void show_crdt(twopset<string>& crdt)
{
  if(!REPL) return;
  l();
  cout << crdt << endl;
  ul();
}

time_t now()
{
  time_t timer;
  time(&timer);
  return timer;
}

void log_message_sent()
{
  if(REPL) return;
  l();
  cout << now() << "|L|" << endl;
  ul();
}

void log_bytes_received(proto::message& message)
{
  if(REPL) return;
  l();
  cout << now();
  if(message.type() == proto::message::TWOPSET)
  {
    cout << "|B|D|";
    twopset<string> crdt;
    message >> crdt;
    for(auto& e : crdt.read())
      cout << e << ",";
    cout << "|";
  }
  else if(message.type() == proto::message::ACK) cout << "|B|A||";
  else if(message.type() == proto::message::ID) cout << "|B|I||";
  else
  {
    cout << "Hmm, there's something wrong xD\n";
    helper::pb::show(message);
    cout << endl;
  }
  cout << message.id() << "|" << message.seq() << "|" << message.ByteSize() << endl;
  ul();
}

void log_new_state(twopset<string>& crdt)
{
  if(REPL) return;
  l();
  cout << now() << "|S|";
  for(const auto& e : crdt.read())
    cout << e << ",";
  cout << endl;
  ul();
}

void log_op(string& op)
{
  if(REPL) return;
  l();
  cout << now() << "|O|" << op << endl; 
  ul();
}
