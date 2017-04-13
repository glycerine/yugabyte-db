#!/usr/bin/env python2
import argparse
import os
import pipes
import subprocess
import sys
import time


def list_command(service):
    return "ps auxww | grep [y]b-{0} | grep -v bash".format(service)


def service_command(service, command):
    return "yb-{0}-ctl.sh {1}".format(service, command)


def stop_command(service):
    return service_command(service, 'stop')


def parse_hosts(inp):
    return [host for host in inp.split(' ') if len(host) > 0]


class ClusterManager(object):
    def __init__(self, args):
        self.master_ips = ClusterManager.get_arg(args, 'master_ips')
        self.tserver_ips = ClusterManager.get_arg(args, 'tserver_ips')
        default_pem = os.path.join(os.environ["HOME"], ".yugabyte/yugabyte-dev-aws-keypair.pem")
        self.pem_file = ClusterManager.get_arg(args, 'pem_file', default_pem)
        self.repo = ClusterManager.get_arg(args, 'repo', '~/code/yugabyte')
        self.tar_prefix = args.tar_prefix
        self.port = ClusterManager.get_arg(args, 'port', 54422)

        self.master_hosts = parse_hosts(self.master_ips)
        self.tserver_hosts = parse_hosts(self.tserver_ips)
        self.all_hosts = list(set(self.master_hosts + self.tserver_hosts))

    @staticmethod
    def setup_parser(parser):
        parser.add_argument('--pem_file',
                            nargs='?',
                            help='name of the pem file')
        parser.add_argument('--master_ips',
                            nargs='*',
                            help="space separated IP of masters (e.g., '10.a.b.c 10.d.e.f')")
        parser.add_argument('--tserver_ips',
                            nargs='*',
                            help="space separated IP of tservers (e.g., '10.a.b.c 10.d.e.f')")
        parser.add_argument('--repo',
                            nargs='?',
                            help="repository base used to pick up TAR file")
        parser.add_argument('--tar_prefix',
                            nargs='?',
                            help="tar file prefix (e.g., "
                                 "yugabyte.2bdf48724db5869d0c88c85e0fa65e9ac3a21511-release)")
        parser.add_argument('--port', type=int, nargs='?', help="ssh port")

    def service_hosts(self, service):
        return getattr(self, service + "_hosts")

    def remote_launch(self, host, command):
        command_args = ['ssh',
                        '-i',
                        self.pem_file,
                        '-p',
                        str(self.port),
                        'centos@{0}'.format(host),
                        command]
        return subprocess.Popen(command_args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def scp(self, source, destination):
        command_args = ['scp', '-i', self.pem_file, '-P', str(self.port), source, destination]
        return subprocess.Popen(command_args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    @staticmethod
    def get_arg(args, key, default=None):
        if not hasattr(args, key) or getattr(args, key) is None:
            if default is None:
                error('Please specify: {0}'.format(key))
            else:
                return default
        return getattr(args, key)

    def launch_simple(self, hosts, command, title):
        return [SimpleProcedure(self, host, command, title) for host in hosts]

    def launch_at_service(self, service, command, title):
        return self.launch_simple(self.service_hosts(service), command, title)

    def execute_service_commands(self, services, commands):
        procedures = []
        for service in services:
            for command in commands:
                procedures += self.launch_simple(self.service_hosts(service),
                                                 service_command(service, command),
                                                 "Perform {0} {1}".format(service, command))
        show_output(procedures)

    def execute_everywhere(self, command):
        show_output(self.launch_simple(self.all_hosts, command, '`{0}`'.format(command)))


help_printer = None


def error(message, print_help=True):
    global help_printer
    sys.stderr.write(message + "\n")
    if print_help:
        help_printer()
    sys.exit(1)


def print_output(process, title, host):
    print("================= {0} at {1} =================".format(title, host))
    for line in process.stdout.readlines():
        sys.stdout.write(line.decode('utf-8'))


class Procedure:
    def check(self):
        pass

    def describe(self):
        pass


class SimpleProcedure(Procedure):
    def __init__(self, manager, host, command, title):
        self.process = manager.remote_launch(host, command)
        self.title = title
        self.host = host
        self.return_code = None

    def check(self):
        if self.process.poll() is not None:
            self.return_code = self.process.returncode
            print_output(self.process, self.title, self.host)
            return True
        return False

    def describe(self):
        return "{0} at {1}".format(self.title, self.host)


class StopProcedure(Procedure):
    RETRY_TIMEOUT_SEC = 1

    def __init__(self, manager, host, service):
        self.manager = manager
        self.host = host
        self.service = service
        self.process = None
        self.done = False
        self.last_stop_sent = None

    def check(self):
        if not self.done:
            self.__do_work()
        return self.done

    def __call_stop(self):
        self.last_stop_sent = None
        self.process = self.__launch(stop_command(self.service))

    def __call_list(self):
        self.process = self.__launch(list_command(self.service))

    def __launch(self, command):
        return self.manager.remote_launch(self.host, command)

    def __do_work(self):
        if self.process is None:
            self.__call_stop()
        if self.process.poll() is not None:
            if self.last_stop_sent is None:
                self.last_stop_sent = time.time()
                self.__call_list()
            else:
                exist = False
                for line in self.process.stdout.readlines():
                    if not line.decode('utf-8').startswith("Warning: Permanently added"):
                        exist = True
                        break
                if not exist:
                    print("Stopped {0} at: {1}".format(self.service, self.host))
                    self.done = True
                elif time.time() > self.last_stop_sent + StopProcedure.RETRY_TIMEOUT_SEC:
                    self.__call_stop()
                else:
                    self.__call_list()

    def describe(self):
        return "Stop {0} at {1}".format(self.service, self.host)


class CopyTarProcedure(Procedure):
    def __init__(self, manager, host):
        self.manager = manager
        self.host = host
        self.step = 0
        print("Copy tar to {0}".format(self.host))
        self.process = manager.scp("{0}/build/{1}.tar.gz".format(manager.repo, manager.tar_prefix),
                                   'centos@{0}:/tmp'.format(host))

    def check(self):
        if self.step == 3:
            return True
        if self.step == 2:
            for line in self.process.stdout.readlines():
                if line.decode('utf-8').startswith('+'):
                    sys.stdout.write("{0}: {1}".format(self.host, line))
        if self.process.poll() is not None:
            if self.process.returncode != 0:
                print_output(self.process, "Copy tar failed", self.host)
                sys.exit(1)
            self.step += 1
            tar_prefix = self.manager.tar_prefix
            if self.step == 1:
                print("Tar copied to {0}".format(self.host))
                command = "sudo -u yugabyte mkdir -p /opt/yugabyte/{0}".format(tar_prefix)
                self.process = self.manager.remote_launch(self.host, command)
            elif self.step == 2:
                print("Extracting tar at {0}".format(self.host))
                command = "cd /opt/yugabyte/{0} && " \
                          "sudo -u yugabyte tar xvf /tmp/{0}.tar.gz && " \
                          "sudo /opt/yugabyte/{0}/bin/post_install.sh".format(tar_prefix)
                self.process = self.manager.remote_launch(self.host, command)
            else:
                print("Tar extracted at {0}".format(self.host))
                return True
        return False

    def describe(self):
        return "Copy tar to {0}".format(self.host)


def show_output(procedures):
    exit_code = 0
    i = 0
    start_time = time.time()
    while i != len(procedures) and time.time() < start_time + 15:
        if procedures[i].check():
            exit_code = procedures[i].return_code if exit_code == 0 else exit_code
            i += 1
        time.sleep(0.1)

    if i != len(procedures):
        error('Timed out: {0}'.format(procedures[i].describe()), False)

    if exit_code != 0:
        sys.exit(exit_code)


def wait_all(procedures, timeout):
    finished = False
    start = time.time()
    while not finished and time.time() < start + timeout:
        finished = False not in [p.check() for p in procedures]
        time.sleep(0.1)
    if not finished:
        for p in procedures:
            if not p.check():
                print("Undone: {0}".format(p.describe()))


class RollTask:
    def __init__(self, manager, host, service, upgrade):
        self.manager = manager
        self.host = host
        self.service = service
        self.upgrade = upgrade
        self.sleep_time = 5 if service == 'master' else 45
        action = "Upgrade" if self.upgrade else "Restart"
        self.description = "{0} {1} at {2}".format(action, self.service, self.host)

    def execute(self):
        print(self.description)
        wait_all([StopProcedure(self.manager, self.host, self.service)], 30)
        if self.upgrade:
            pattern = "sudo -u yugabyte rm /opt/yugabyte/{0} && " \
                      "sudo -u yugabyte ln -s /opt/yugabyte/{1} /opt/yugabyte/{0}"
            self.__execute(pattern.format(self.service, self.manager.tar_prefix), "Update link")
        self.__execute(service_command(self.service, 'start'), "Start {0}".format(self.service))

    def __execute(self, command, title):
        show_output(self.manager.launch_simple([self.host], command, title))


def perform_tasks(tasks):
    sleep_time = None
    for task in tasks:
        if sleep_time is not None:
            print("Sleeping {0} seconds before going to {1}...".format(sleep_time,
                                                                       task.description))
            time.sleep(sleep_time)
        task.execute()
        sleep_time = task.sleep_time


class Command:
    def __init__(self, name, action, hint):
        self.name = name
        self.action = action
        self.hint = hint


class YBControl:
    def __init__(self):
        global help_printer
        self.all_services = ['master', 'tserver']
        self.manager = None
        self.commands = {}
        self.__fill_commands()
        self.__parse_arguments()
        help_printer = self.__print_help

    def stop_services(self, services):
        procedures = []
        for service in services:
            procedures += [StopProcedure(self.manager, host, service)
                           for host in self.manager.service_hosts(service)]
        wait_all(procedures, 30)

    def roll_servers(self, services, upgrade):
        tasks = []
        for service in services:
            tasks += [RollTask(self.manager, host, service, upgrade)
                      for host in self.manager.service_hosts(service)]
        perform_tasks(tasks)

    def copy_tar(self):
        if self.manager.tar_prefix is None:
            error('Please specify: tar_prefix')
        wait_all([CopyTarProcedure(self.manager, host) for host in self.manager.all_hosts], 90)

    def masters_create(self):
        self.manager.execute_service_commands(['master'], ['create'])

    def status(self):
        procedures = []
        for service in self.all_services:
            procedures += self.manager.launch_at_service(service,
                                                         list_command(service),
                                                         "Processes related to {0}".format(service))
        show_output(procedures)

    def start_services(self, services):
        self.manager.execute_service_commands(services, ['start'])

    def clean_services(self, services):
        self.manager.execute_service_commands(services, ['clean', 'clean-logs'])

    def execute(self):
        parameters = self.parameters
        if len(parameters) == 0:
            error("Please specify remote command")
        elif len(parameters) > 1:
            parameters = " ".join([pipes.quote(p) for p in parameters])
        else:
            parameters = parameters[0]
        self.manager.execute_everywhere(parameters)

    def __add_command_ex(self, name, action, hint):
        self.commands[name] = Command(name, action, hint)

    def __add_command(self, name, hint):
        action = getattr(self, name)
        self.__add_command_ex(name, action, hint)

    def __add_commands(self, suffix, desc):
        impl = getattr(self, "{0}_services".format(suffix))
        self.__add_command_ex('masters_' + suffix,
                              lambda: impl(['master']),
                              desc.format("the YB master"))
        self.__add_command_ex('tservers_' + suffix,
                              lambda: impl(['tserver']),
                              desc.format("the YB tserver"))
        self.__add_command_ex('all_' + suffix,
                              lambda: impl(self.all_services),
                              desc.format("all YB"))

    def __fill_commands(self):
        self.__add_command('execute', "Execute a command on all hosts.")
        self.__add_command('masters_create',
                           'Start the YB master processes for the cluster in cluster create mode.')
        self.__add_command('status', 'Status of masters and servers.')
        self.__add_command('copy_tar', 'Copy the tar file to all the nodes.')
        self.__add_commands('stop', "Stop {0} processes.")
        self.__add_commands('start', "Start {0} processes.")
        self.__add_commands('clean', "Clean {0} data and logs.")

        for upgrade in [False, True]:
            name_suffix = '_rolling_{0}'.format("upgrade" if upgrade else "restart")
            if upgrade:
                hint = "Restarts the {0} in a rolling manner."
            else:
                hint = "Upgrades the {0} to the newly copied TAR in a rolling manner."
            for service in self.all_services:
                self.__add_command_ex(service + 's' + name_suffix,
                                      lambda s=service, u=upgrade: self.__roll_servers([s], u),
                                      hint.format(service + 's'))
            self.__add_command_ex('all' + name_suffix,
                                  lambda u=upgrade: self.roll_servers(self.all_services, u),
                                  hint.format('all'))

    def __print_help(self):
        self.parser.print_help()
        print("Commands:")
        commands_list = sorted(self.commands)
        max_len = max([len(x) for x in commands_list])
        for command in commands_list:
            print(("  {0: <" + str(max_len) + "} - {1}").format(command,
                                                                self.commands[command].hint))

    def __parse_arguments(self):
        parser = argparse.ArgumentParser(description='YB Control.')
        self.parser = parser
        ClusterManager.setup_parser(parser)

        parser.add_argument('command', help="command to execute")
        parser.add_argument('parameters', nargs=argparse.REMAINDER, help="command arguments")

        args = parser.parse_args()

        for key in dir(args):
            if not key.startswith("__") and getattr(args, key) is None:
                env_key = 'YB_' + key.upper()
                if env_key in os.environ:
                    setattr(args, key, os.environ[env_key])

        self.manager = ClusterManager(args)
        self.command = args.command
        self.parameters = args.parameters

    def run(self):
        if not self.command:
            error("Please specify command")
        elif self.command in self.commands:
            self.commands[self.command].action()
        else:
            error("Command not found: {0}".format(self.command))


if __name__ == "__main__":
    YBControl().run()
