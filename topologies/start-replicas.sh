#!/usr/bin/env python

import sys
from os import system
from sets import Set
from os import listdir
from os.path import isfile, join

if (len(sys.argv) < 4):
  print "Usage: " + sys.argv[0] + " REPLICAS_CONFIG REPLICAS_COMMANDS_DIR LOGS_DIR [GOSSIP_SLEEP_TIME]"
  sys.exit()

gossip_sleep_time = 10
replicas_config = sys.argv[1]
replicas_commands_dir = sys.argv[2]
logs_dir = sys.argv[3]
if(len(sys.argv) > 4):
  gossip_sleep_time = sys.argv[4]

config = {}
commands_files = [f for f in listdir(replicas_commands_dir) if isfile(join(replicas_commands_dir, f))]

with open(replicas_config, "r") as file:
  for line in file:
    id,host,port = (i.rstrip("\n") for i in line.split(" "))
    config[id] = [host, port]

print "rm .PIDS"
print "rm log/*.log"

for commands in commands_files:
  replica_id = commands.split(".")[0]
  if (replica_id == ""):
    continue
  replica_port = config[replica_id][1]

  cmd = "./../replica"
  info = replica_id + ":" + replica_port
  output = "2>&1 > " + logs_dir + replica_id + ".log"
  input = "< " + replicas_commands_dir + commands

  start_command = "{cmd} {info} -s {gossip_sleep_time} {output} {input} &".format(cmd = cmd, info = info, gossip_sleep_time = gossip_sleep_time, output = output, input = input)
  print start_command
  print "echo $! >> .PIDS"
