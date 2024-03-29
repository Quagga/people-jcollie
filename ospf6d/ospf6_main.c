/*
 * Copyright (C) 1999 Yasuhiro Ohara
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the 
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330, 
 * Boston, MA 02111-1307, USA.  
 */

#include <zebra.h>
#include <lib/version.h>

#include "getopt.h"
#include "thread.h"
#include "log.h"
#include "command.h"
#include "vty.h"
#include "memory.h"
#include "if.h"
#include "filter.h"
#include "prefix.h"
#include "plist.h"
#include "privs.h"
#include "sigevent.h"

#include "ospf6d.h"

/* Default configuration file name for ospf6d. */
#define OSPF6_DEFAULT_CONFIG       "ospf6d.conf"

/* Default port values. */
#define OSPF6_VTY_PORT             2606

/* ospf6d privileges */
zebra_capabilities_t _caps_p [] =
{
  ZCAP_NET_RAW,
  ZCAP_BIND
};

struct zebra_privs_t ospf6d_privs =
{
#if defined(QUAGGA_USER)
  .user = QUAGGA_USER,
#endif
#if defined QUAGGA_GROUP
  .group = QUAGGA_GROUP,
#endif
#ifdef VTY_GROUP
  .vty_group = VTY_GROUP,
#endif
  .caps_p = _caps_p,
  .cap_num_p = 2,
  .cap_num_i = 0
};

/* ospf6d options, we use GNU getopt library. */
struct option longopts[] = 
{
  { "daemon",      no_argument,       NULL, 'd'},
  { "config_file", required_argument, NULL, 'f'},
  { "pid_file",    required_argument, NULL, 'i'},
  { "vty_addr",    required_argument, NULL, 'A'},
  { "vty_port",    required_argument, NULL, 'P'},
  { "user",        required_argument, NULL, 'u'},
  { "group",       required_argument, NULL, 'g'},
  { "version",     no_argument,       NULL, 'v'},
  { "dryrun",      no_argument,       NULL, 'C'},
  { "help",        no_argument,       NULL, 'h'},
  { 0 }
};

/* Configuration file and directory. */
char config_default[] = SYSCONFDIR OSPF6_DEFAULT_CONFIG;

/* ospf6d program name. */
char *progname;

/* is daemon? */
int daemon_mode = 0;

/* Master of threads. */
struct thread_master *master;

/* Process ID saved for use by init system */
const char *pid_file = PATH_OSPF6D_PID;

/* Help information display. */
static void
usage (char *progname, int status)
{
  if (status != 0)
    fprintf (stderr, "Try `%s --help' for more information.\n", progname);
  else
    {    
      printf ("Usage : %s [OPTION...]\n\n\
Daemon which manages OSPF version 3.\n\n\
-d, --daemon       Runs in daemon mode\n\
-f, --config_file  Set configuration file name\n\
-i, --pid_file     Set process identifier file name\n\
-A, --vty_addr     Set vty's bind address\n\
-P, --vty_port     Set vty's port number\n\
-u, --user         User to run as\n\
-g, --group        Group to run as\n\
-v, --version      Print program version\n\
-C, --dryrun       Check configuration for validity and exit\n\
-h, --help         Display this help and exit\n\
\n\
Report bugs to zebra@zebra.org\n", progname);
    }

  exit (status);
}

/* SIGHUP handler. */
void 
sighup (void)
{
  zlog_info ("SIGHUP received");
}

/* SIGINT handler. */
void
sigint (void)
{
  zlog_notice ("Terminating on signal SIGINT");
  exit (0);
}

/* SIGTERM handler. */
void
sigterm (void)
{
  zlog_notice ("Terminating on signal SIGTERM");
  exit (0);
}

/* SIGUSR1 handler. */
void
sigusr1 (void)
{
  zlog_info ("SIGUSR1 received");
  zlog_rotate (NULL);
}

struct quagga_signal_t ospf6_signals[] =
{
  {
    .signal = SIGHUP,
    .handler = &sighup,
  },
  {
    .signal = SIGINT,
    .handler = &sigint,
  },
  {
    .signal = SIGTERM,
    .handler = &sigterm,
  },
  {
    .signal = SIGUSR1,
    .handler = &sigusr1,
  },
};

/* Main routine of ospf6d. Treatment of argument and starting ospf finite
   state machine is handled here. */
int
main (int argc, char *argv[], char *envp[])
{
  char *p;
  int opt;
  char *vty_addr = NULL;
  int vty_port = 0;
  char *config_file = NULL;
  struct thread thread;
  int dryrun = 0;

  /* Set umask before anything for security */
  umask (0027);

  /* Preserve name of myself. */
  progname = ((p = strrchr (argv[0], '/')) ? ++p : argv[0]);

  /* Command line argument treatment. */
  while (1) 
    {
      opt = getopt_long (argc, argv, "df:i:hp:A:P:u:g:vC", longopts, 0);
    
      if (opt == EOF)
        break;

      switch (opt) 
        {
        case 0:
          break;
        case 'd':
          daemon_mode = 1;
          break;
        case 'f':
          config_file = optarg;
          break;
        case 'A':
          vty_addr = optarg;
          break;
        case 'i':
          pid_file = optarg;
          break;
        case 'P':
         /* Deal with atoi() returning 0 on failure, and ospf6d not
             listening on ospf6d port... */
          if (strcmp(optarg, "0") == 0)
            {
              vty_port = 0;
              break;
            }
          vty_port = atoi (optarg);
          vty_port = (vty_port ? vty_port : OSPF6_VTY_PORT);
          break;
        case 'u':
          ospf6d_privs.user = optarg;
          break;
	case 'g':
	  ospf6d_privs.group = optarg;
	  break;
        case 'v':
          print_version (progname);
          exit (0);
          break;
	case 'C':
	  dryrun = 1;
	  break;
        case 'h':
          usage (progname, 0);
          break;
        default:
          usage (progname, 1);
          break;
        }
    }

  /* thread master */
  master = thread_master_create ();

  /* Initializations. */
  zlog_default = openzlog (progname, ZLOG_OSPF6,
                           LOG_CONS|LOG_NDELAY|LOG_PID,
                           LOG_DAEMON);
  zprivs_init (&ospf6d_privs);
  /* initialize zebra libraries */
  signal_init (master, Q_SIGC(ospf6_signals), ospf6_signals);
  cmd_init (1);
  vty_init (master);
  memory_init ();
  if_init ();
  access_list_init ();
  prefix_list_init ();

  /* initialize ospf6 */
  ospf6_init ();

  /* sort command vector */
  sort_node ();

  /* parse config file */
  vty_read_config (config_file, config_default);

  /* Start execution only if not in dry-run mode */
  if (dryrun)
    return(0);
  
  if (daemon_mode)
    daemon (0, 0);

  /* pid file create */
  pid_output (pid_file);

  /* Make ospf6 vty socket. */
  if (!vty_port)
    vty_port = OSPF6_VTY_PORT;
  vty_serv_sock (vty_addr, vty_port, OSPF6_VTYSH_PATH);

  /* Print start message */
  zlog_notice ("OSPF6d (Quagga-%s ospf6d-%s) starts: vty@%d",
               QUAGGA_VERSION, OSPF6_DAEMON_VERSION,vty_port);

  /* Start finite state machine, here we go! */
  while (thread_fetch (master, &thread))
    thread_call (&thread);

  /* Log in case thread failed */
  zlog_warn ("Thread failed");

  /* Not reached. */
  exit (0);
}


