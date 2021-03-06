#!/usr/bin/env python

import sys
from os import system
from sets import Set
from random import randint

wait_time_between_updates = 5
rounds_of_updates = 42
max_update_number = 100 * 1000 * 1000

def random_update():
  return "add " + str(randint(0, max_update_number)) + "\n"

if (len(sys.argv) < 5):
  print "Usage: " + sys.argv[0] + " REPLICAS_CONFIG TOPOLOGY_CONFIG REPLICAS_COMMANDS_DIR UPDATES_PER_ROUND"
  sys.exit()

replicas_config = sys.argv[1]
topology_config = sys.argv[2]
replicas_commands_dir = sys.argv[3]
updates_per_round = int(sys.argv[4])

system("rm -f " + replicas_commands_dir + "*.cmds")

config = {}
commands = {}
replicas = Set()

with open(replicas_config, "r") as file:
  for line in file:
    id,host,port = (i.rstrip("\n") for i in line.split(" "))
    config[id] = [host, port]

with open(topology_config, "r") as file:
  for line in file:
    fst,snd = (i.rstrip("\n") for i in line.split(" "))

    replicas.add(fst)
    replicas.add(snd)

    snd_config = config[snd]
    snd_host = snd_config[0]
    snd_port = snd_config[1]
    connect = "connect " + snd + ":" + snd_host + ":" + snd_port

    if(fst in commands):
      commands[fst].append(connect)
    else:
      commands[fst] = [connect]

for replica_id, connects in commands.iteritems():
  replicas.remove(replica_id)
  with open(replicas_commands_dir + replica_id + ".cmds", "w") as file:
    file.write("wait 5\n")
    for connect in connects:
      file.write(connect + "\n")

    for i in range(0, rounds_of_updates):
      file.write("wait " + str(wait_time_between_updates) + "\n")
      for j in range(0, updates_per_round):
        file.write(random_update())

# if replicas is empty, then I have at least one command for each replica
# if not, then there are replicas that do not have any command
# but I should create the command file even so
for replica_id in replicas:
  with open(replicas_commands_dir + replica_id + ".cmds", "w") as file:
    file.write("\n")


