#!/usr/bin/env python3
import os
import sys
import argparse
import concurrent.futures
import subprocess
import json
from socket import gethostname
from pathlib import Path

sys.path.append('/share/micron/rapid/install/gcc-release/bin/')
import rapid

# JS: This is for the current twosisters setup.  App must exist on hosts[0]!
# hosts = ["twosisters2", "twosisters"] #, "twosisters"]
hosts = ["twosisters"]

# JS: Path to util
rapidutil = "/share/micron/rapid/install/gcc-release/bin/rapidutil"

# JS: Micron environment path
rapidenv = "/share/micron/environment.sh"

# JS: This is to turn print all on or off
defaultVerbosity = False

def empty_region(name, verbose):
    """
    If region already exists, deletes all its items.

    Parameters
    ----------
    name : str
        The name of the region to create
    verbose : bool
        Flag to print region items

    Returns
    -------
    region
        Empty rapid region object or None
    """
    region = rapid.lookup_region(name)
    if region is not None:
        if verbose:
            print("Deleting:", region.list_items())
        items = region.list_items().value
        for item in items:
            region.lookup_item(item).seek_and_adstroy()
    return region

def alloc_region_for_app(name, size, alignment, verbose=defaultVerbosity):
    """
    Creates FAM region using rapid library.  The region is impersistent and accessible meaning
    the region will persist as long as there are references to it and is visible via rapid's
    lookup apis

    Parameters
    ----------
    name : str
        The name of the region to create
    size : int
        Size of the region to create
    alignment : int
        Alignment of the region to create
    verbose : bool
        Flag to print info about the region created

    Returns
    -------
    region
        Empty rapid region object
    """
    region = empty_region(name, verbose)
    if region is None:
        # flags = rapid.RegionFlags.IMPERSISTENT | rapid.RegionFlags.ACCESSIBLE
        flags = rapid.RegionFlags.ACCESSIBLE # debug
        region = rapid.create_aligned_region(name, alignment, size+64, flags) # Pad by 64
        # region = rapid.create_aligned_region(name, alignment, size, flags)
    print("Status:",region.status())
    assert region.status() == rapid.FamStatus.FAM_NO_ERROR
    # assert region.size() == size
    assert region.size() == size + 64
    region.create_aligned_item(64, 64, "padding")
    if verbose:
        print("Name:", region.name(), "Size:", region.size(), "Ref Count:", region.ref_count())
    return region

class command:
    """
    This class provides a wrapper around subprocesses.
    """
    def __init__(self, host, cmds, wait=True, verbose=defaultVerbosity):
        """
        Parameters
        ----------
        host : str
            If host is provided will ssh commands onto indicated host.  If none is provided, will run locally.
        cmds : list of str
            List of strings to run
        wait : bool
            Flag indicating to run the commands immediately
        verbose : bool
            Flag to print commands and output
        """
        if host is None:
            self.cmd = ";".join(cmds)
        else:
            cmds.insert(0, "<<'EOL'")
            cmds.append("EOL")
            cmd_str = "\n".join(cmds)
            self.cmd = " ".join(["ssh", host, "-T", cmd_str])
        if verbose:
            print(self.cmd)
        self.sp = subprocess.Popen(self.cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        self.out = None
        self.err = None
        if wait:
            self.wait()

    def wait(self):
        """
        This will wait for the command(s) to finish executing and save the output
        """
        self.out, self.err = self.sp.communicate()

    def retCode(self):
        """
        Returns the return code from the command executed
        """
        return self.sp.returncode

def run_pool(funct, run_hosts, verbose=defaultVerbosity):
    """
    This function launches a thread per run_host.  While the python GIL only lets one thread
    run at a time, we are launching functions to run on different hosts, that can run in
    "parallel."
    See https://docs.python.org/3/library/concurrent.futures.html

    Parameters
    ----------
    funct : function
        This is a function that will be run asychronously
    run_hosts : list of strs
        Names of the hosts to run commands on
    verbose : bool
        Flag to print results of the function
    """
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(run_hosts)) as executor:
        future_to_host = {executor.submit(funct, host): host for host in run_hosts}
        for future in concurrent.futures.as_completed(future_to_host):
            host = future_to_host[future]
            if verbose:
                print(host, future.result())

def copy_app_on_hosts(app, run_dir, app_runner):
    """
    Copies the app executable on each host.

    Parameters
    ----------
    app : str
        Path of the executable to copy
    run_dir : str
        Path to copy executable to
    app_runner :
        Executable path in the run directory
    """
    def funct(host):
        """
        This is the function used by run_pool.  It contains the copy commands.
        The app is copied from hosts[0] to all others.

        Parameters
        ----------
        host : str
            Name of the host

        Returns
        -------
        str
            The output of the copy command
        """
        cmds = [
            "mkdir -p " + run_dir,
            "scp " + hosts[0] + ":" + app + " " + app_runner,
        ]
        if gethostname() == host:
            host = None
            cmds[1] = "cp " + app + " " + app_runner
        cmd = command(host, cmds, wait=True)
        return cmd.err + cmd.out
    run_pool(funct, hosts, verbose=False)

def run_app_on_hosts(app_runner, app_args, run_hosts, verbose=defaultVerbosity):
    """
    This function launches app w/o MPI using the run_pool function.

    Parameters
    ----------
    app_runner : str
        Path to the executable to run
    app_args : str
        Args to give executable
    run_hosts : list
        List of the hosts to run on
    verbose : bool
        Flag to print runner
    """
    if verbose:
        print("SSH RUNNER", flush=True)
    def funct(host):
        """
        This is the function used by run_pool.  It contains the launcher commands.

        Parameters
        ----------
        host : str
            Name of the host

        Returns
        -------
        str
            The output of the app
        """
        cmds = [
            "source " + rapidenv,
            app_runner + " " + app_args
        ]
        host = host if gethostname() != host else None
        cmd = command(host, cmds, wait=True)
        return cmd.err + cmd.out
    run_pool(funct, run_hosts, verbose=True)

def mpi_run(app_runner, app_args, app_output, run_hosts, verbose=defaultVerbosity):
    """
    This launches an app using MPI.

    Parameters
    ----------
    app_runner : str
        Path to the executable to run
    app_args : str
        Args to give executable
    app_output : str
        Path to output.  This is needed if app fails and cmd.out and cmd.err get nothing.
    run_hosts : list
        List of the hosts to run on
    verbose : bool
        Flag to print runner

    Returns
    -------
    int
        The return code of the MPI Job.
    """
    if verbose:
        print("MPI RUNNER, {}".format(app_output), flush=True)
    cmd = ["mpirun -np {} --host {} -x BADSSERVER=twosisters2:50505 {} {} &> {}".format(len(run_hosts), ",".join(run_hosts), app_runner, app_args, app_output)]
    cmd = command(None, cmd, verbose=True)
    if verbose:
        print(cmd.out)
        print(cmd.err)
    return cmd.retCode()

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--shared-region-name", help="Name of the shared region", type=str, default="shared") #"/tmp/")
    parser.add_argument("--fam-ranks-region-name", help="Name of fam rank region used to determine rank w/o MPI", type=str, default="fam_ranks")
    parser.add_argument("--alignment", help="Memory alignment of shared memory region", type=int, default=64)
    # parser.add_argument("--region-size", help="Size of shared memory region", type=int, default=2*1024*1024*1024)
    parser.add_argument("--region-size", help="Size of shared memory region", type=int, default=10*1024*1024*1024)
    parser.add_argument("--run-dir", help="Directory to run executables from", type=str, default=str(Path.home())+"/runners") #"/tmp/")
    parser.add_argument("--path-to-json", help="Path to json with app args", type=str, default="./args.json")
    parser.add_argument("--mpi-runner", help="Will use mpi to run app", action='store_true', default=False)
    parser.add_argument("--output-file", help="File for application output", type=str, default="logger.txt")
    parser.add_argument("--exe", help="Use this to bypass json. Supports no args to exe.", type=str, default="")
    parser.add_argument("--ranks", help="Number of ranks to run on.  If more than hosts, will oversubscribe.", type=int, default=len(hosts))
    args = parser.parse_args()

    if args.exe == "":
        try:
            with open(args.path_to_json) as file:
                js = json.load(file)

                # JS: App parameters
                app = js["app"]
                app_args = js["args"]

                # JS: Shared memory init parameters
                init = js["init"] if "init" in js else None
                init_args = js["init_args"] if "init" in js else None
        except:
            print("Error reading", args.path_to_json)
            exit(1)
    else:
        app = args.exe
        app_args = ""
        init = None
        init_args = None

    # JS: Turn app to full paths
    # app_name = os.path.basename(app)
    app_runner = os.path.abspath(app)
    print("TO RUN:", app_runner)

    # JS: Create regions
    rank_region = alloc_region_for_app(args.fam_ranks_region_name, 1024, 8)
    shared_region = alloc_region_for_app(args.shared_region_name, args.region_size, args.alignment)

    # JS: Print created pre-app regions
    if defaultVerbosity and init is not None:
        initStr = init + " " + init_args if init_args is not None else init
        cmd = command(None, [rapidutil + " -L", initStr], wait=True, verbose=True)
        print(cmd.err, cmd.out)

    # JS: Copy our app to our hosts
    # copy_app_on_hosts(app, args.run_dir, app_runner)

    if len(hosts) == args.ranks:
        run_hosts = hosts
    elif args.ranks < len(hosts):
        run_hosts = hosts[:args.ranks]
    else:
        run_hosts = hosts * args.ranks
        run_hosts = run_hosts[:args.ranks]
    if defaultVerbosity:
        print("HOSTS:", run_hosts)

    # JS: run our app.  Only supporting return codes from MPI...
    if args.mpi_runner:
        retCode = mpi_run(app_runner, app_args, args.output_file, run_hosts)
    else:
        run_app_on_hosts(app_runner, app_args, run_hosts)
        retCode = 0

    if defaultVerbosity:
        cmd = command(None, [rapidutil + " -L"], wait=True, verbose=True)
        print(cmd.err, cmd.out)

    # JS: Release our retions
    rank_region.release()
    shared_region.release()
    print("DONE!", retCode, flush=True)
    exit(retCode)
