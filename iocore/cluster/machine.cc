/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <fcntl.h>
#include "Diags.h"
#include "global.h"
#include "nio.h"
#include "connection.h"
#include "message.h"
#include "machine.h"
#include "ink_config.h"
#include "P_Cluster.h"

unsigned int my_machine_ip = 0;
int cluster_machine_count = 0; //total machine count of the cluster

ClusterMachine *cluster_machines = NULL;
static ClusterMachine **sorted_machines = NULL;  //sort by ip and port
static ink_mutex machine_lock;

static ClusterMachine *do_add_machine(ClusterMachine *m, int *result);

ClusterMachine *add_machine(const unsigned int ip, const int port)
{
  ClusterMachine machine;
  struct in_addr in;
  int result;
  char *ip_addr;

  memset((void *)&machine, 0, sizeof(machine));
  in.s_addr = ip;
  ip_addr = inet_ntoa(in);
  machine.hostname_len = strlen(ip_addr);
  machine.hostname = strdup(ip_addr);
  machine.cluster_port = port;
  machine.ip = ip;

  return do_add_machine(&machine, &result);
}

int init_machines()
{
  int result;
  int bytes;

  if ((result=ink_mutex_init(&machine_lock, "machine_lock")) != 0) {
    return result;
  }

  cluster_machine_count = 0;
  bytes = sizeof(ClusterMachine) * MAX_MACHINE_COUNT;
  cluster_machines = (ClusterMachine *)malloc(bytes);
  if (cluster_machines == NULL) {
    Error("file: "__FILE__", line: %d, "
        "malloc %d bytes fail!", __LINE__, bytes);
    return ENOMEM;
  }
  memset((void *)cluster_machines, 0, bytes);

  bytes = sizeof(ClusterMachine *) * MAX_MACHINE_COUNT;
  sorted_machines = (ClusterMachine **)malloc(bytes);
  if (sorted_machines == NULL) {
    Error("file: "__FILE__", line: %d, "
        "malloc %d bytes fail!", __LINE__, bytes);
    return ENOMEM;
  }
  memset((void *)sorted_machines, 0, bytes);

  return 0;
}

static int compare_machine(const void *p1, const void *p2)
{
  const ClusterMachine **m1 = (const ClusterMachine **)p1;
  const ClusterMachine **m2 = (const ClusterMachine **)p2;

  if ((*m1)->ip == (*m2)->ip) {
    return (*m1)->cluster_port - (*m2)->cluster_port;
  }
  else {
    return (*m1)->ip < (*m2)->ip ? -1 : 1;
  }
}

static ClusterMachine *do_add_machine(ClusterMachine *m, int *result)
{
  ClusterMachine **ppMachine;
  ClusterMachine **ppMachineEnd;
  ClusterMachine **pp;
  ClusterMachine *pMachine;
  int cr;

  cr = -1;
  ink_mutex_acquire(&machine_lock);
  ppMachineEnd = sorted_machines + cluster_machine_count;
  for (ppMachine=sorted_machines; ppMachine<ppMachineEnd; ppMachine++) {
    cr = compare_machine(&m, ppMachine);
    if (cr <= 0) {
      break;
    }
  }

  do {
    if (cr == 0) {  //found
      pMachine = *ppMachine;
      *result = EEXIST;
      break;
    }

    if (cluster_machine_count >= MAX_MACHINE_COUNT) {
      Error("file: "__FILE__", line: %d, "
          "host: %s:%u, exceeds max machine: %d!", __LINE__, m->hostname,
          m->cluster_port, MAX_MACHINE_COUNT);
      *result = ENOSPC;
      pMachine = NULL;
      break;
    }

    for (pp=ppMachineEnd; pp>ppMachine; pp--) {
      *pp = *(pp - 1);
    }

    pMachine = cluster_machines + cluster_machine_count;  //the last emlement
    *ppMachine = pMachine;

    pMachine->dead = true;
    pMachine->ip = m->ip;
    pMachine->cluster_port = m->cluster_port;
    pMachine->hostname_len = m->hostname_len;
    if (m->hostname_len == 0) {
      pMachine->hostname = NULL;
    }
    else {
      pMachine->hostname = (char *)malloc(m->hostname_len + 1);
      if (pMachine->hostname == NULL) {
        Error("file: "__FILE__", line: %d, "
            "malloc %d bytes fail!", __LINE__, m->hostname_len + 1);
        *result = ENOMEM;
        break;
      }
      memcpy(pMachine->hostname, m->hostname, m->hostname_len + 1);
    }

    cluster_machine_count++;
    *result = 0;
  } while (0);

  ink_mutex_release(&machine_lock);
  return pMachine;
}

ClusterMachine *get_machine(const unsigned int ip, const int port)
{
  ClusterMachine machine;
  ClusterMachine *target;
  ClusterMachine **found;

  memset((void *)&machine, 0, sizeof(machine));
  machine.ip = ip;
  machine.cluster_port = port;
  target = &machine;
  found = (ClusterMachine **)bsearch(&target, sorted_machines, cluster_machine_count,
      sizeof(ClusterMachine *), compare_machine);
  if (found != NULL) {
    return *found;
  }
  else {
    return NULL;
  }
}

int machine_up_notify(ClusterMachine *machine)
{
  if (machine == NULL) {
    return ENOENT;
  }

  ink_mutex_acquire(&machine_lock);

  Debug(CLUSTER_DEBUG_TAG, "file: "__FILE__", line: %d, "
      "machine_up_notify, %s connection count: %d, dead: %d",
      __LINE__, machine->hostname, machine->now_connections, machine->dead);

  if (machine->dead) {
    machine->dead = false;
    cluster_machine_change_notify(machine);
  }
  ink_mutex_release(&machine_lock);

  return 0;
}

int machine_add_connection(SocketContext *pSockContext)
{
  int result;
  int count;

  ink_mutex_acquire(&machine_lock);
  if ((result=nio_add_to_epoll(pSockContext)) != 0) {
    ink_mutex_release(&machine_lock);
    return result;
  }

  count = ++pSockContext->machine->now_connections;
  ink_mutex_release(&machine_lock);

  Debug(CLUSTER_DEBUG_TAG, "file: "__FILE__", line: %d, "
      "%s add %c connection count: %d, dead: %d", __LINE__,
      pSockContext->machine->hostname, pSockContext->connect_type,
      count, pSockContext->machine->dead);

  return 0;
}

int machine_remove_connection(SocketContext *pSockContext)
{
  int count;
  int result;

  ink_mutex_acquire(&machine_lock);
  if ((result=remove_machine_sock_context(pSockContext)) != 0) {
    ink_mutex_release(&machine_lock);
    return result;
  }

  count = --pSockContext->machine->now_connections;
  if (count == 0 && !pSockContext->machine->dead) { //should remove machine from config
    pSockContext->machine->dead = true;
    cluster_machine_change_notify(pSockContext->machine);
  }
  ink_mutex_release(&machine_lock);

  Debug(CLUSTER_DEBUG_TAG, "file: "__FILE__", line: %d, "
      "%s remove %c connection count: %d, dead: %d", __LINE__,
      pSockContext->machine->hostname, pSockContext->connect_type,
      count, pSockContext->machine->dead);

  return 0;
}

