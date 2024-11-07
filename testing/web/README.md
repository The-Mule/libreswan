# Example

See: http://testing.libreswan.org/


# Manual Setup

To publish the results from running `make kvm-results` as a web page,
point the make variable `WEB_SUMMARYDIR` at the web page's HTML
directory.  For instance, either by adding it to Makefile.inc.local:

   $ mkdir /var/www/html/results
   echo WEB_SUMMARYDIR=/var/www/html/results >> Makefile.inc.local


# Automated Testing

## Setup

Lets assume things are being setup to track the MAIN branch and that
all the files (other than the web pages) will live under:

      base                                    |  ~/main/
      repository/directory under test         |  ~/main/rutdir/
      repository/directory driving the tests  |  ~/main/benchdir/
      directory containing VM disks et.al.    |  ~/main/pooldir/

      directory containing published results  |  ~/results/

and optionally:

      tmpfs containing test vm disks          | /tmp/localdir
      only used when rebuilding the world     | ~/main/scratchdir/

- check that the host machine is correctly configured, see:

      https://libreswan.org/wiki/Test_Suite_-_KVM#Preparing_the_host_machine

- create the html directory where the results will be published

  For instance, to set up results/:

      mkdir ~/results
      mkdir ~/main

- create the pool directory for storing permanent VM disk images

  For instance, assuming building and testing is being run from the
  sub-directory main/:

      cd ~/main/
      mkdir -p pooldir/

- create a directory containing the Repository Under Test (RUT) (ak
  rutdir/)

  In addition to regular updates using "git fetch + git rebase", this
  repository is switched to the commit being tested using "git reset
  --hard".

      cd ~/main/
      git clone https://github.com/libreswan/libreswan.git -b $(basename $PWD) rutdir/

- create a directory containing the Test Bench (web sources and
  scripts aka benchdir/):

      cd ~/main/
      git clone https://github.com/libreswan/libreswan.git -b $(basename $PWD) benchdir/

- configure the Test Bench (benchdir/):

  increase the number of test domains giving them unique prefixes (so
  that they don't run with the default domain names); add WIP to what
  should be tested; and use /tmp/pool:

      cd ~/main/
      echo 'KVM_PREFIXES=m1. m2.'           >> benchdir/Makefile.inc.local
      echo 'KVM_TEST_STATUS += wip'         >> benchdir/Makefile.inc.local
      echo 'KVM_LOCALDIR=/tmp/localdir'     >> benchdir/Makefile.inc.local
      echo 'KVM_RUTDIR='"${PWD}"'/rutdir'   >> benchdir/Makefile.inc.local
      echo 'KVM_POOLDIR='"${PWD}"'/pooldir' >> benchdir/Makefile.inc.local
      echo 'KVM_WEBDIR='"${HOME}"'/results' >> benchdir/Makefile.inc.local

      echo '# cover all bases'              >> benchdir/Makefile.inc.local
      echo 'WEB_SUMMARYDIR=$(KVM_WEBDIR)'   >> benchdir/Makefile.inc.local
      echo 'LSW_WEBDIR=$(KVM_WEBDIR)'       >> benchdir/Makefile.inc.local


## Running

Assuming results are to be published in the directory ~/results/ (see
above), the testing script is invoked as:

      cd ~/main
      cp /dev/null nohup.out ; nohup benchdir/testing/web/tester.sh & tail -f nohup.out


## Restarting and Maintenance

The following things seem to go wrong:

- over time, the test results can get worse

  The symptom is an increasing number of "unresolved" test results
  with an error of "output-missing".  It happens because the domain
  took too long (more than 2 minutes!) to boot.

  tester.sh works around this by detecting the problem and then
  rebuilding domains, but sometimes even that doesn't work so things
  need to be cleaned up.

- the build crashes

  For instance a compiler error, or something more serious such as as
  corrupt VM.

  To mitigate this cascading, after a build failure, tester.sh will
  reset itself and wait for new changes before attempting a further
  build

- the disk fills up

  Test result directory can be pruned without a restart. Once the
  current run finishes, runner.sh will re-build the web pages removing
  the deleted directories (you just need to wait).

  Included in the restart instructions below are suggests for how to
  find directories that should be pruned.

If a restart is required, the following are the recommended steps (if
you're in a hurry, reboot the machine then skip all the way to the end
with "restart"):

- if necessary, crash the existing runner.sh:

  while killing runner.sh et.al. works, it is easier/quicker to just
  crash it by running the following a few times:

      $ cd main/
      $ ( cd rutdir/ && make kvm-uninstall )

- (optional, but recommended) upgrade and reboot the test machine:

      $ sudo dnf upgrade -y
      $ sudo reboot

- (optional) cleanup and update the rutdir/ (tester.sh will do this
  anyway)

      $ cd main/
      $ ( cd rutdir/ && git clean -f )
      $ ( cd rutdir/ && git pull --ff-only )

- (optional) update the benchdir/ repository:

  Remember to first check for local changes:

      $ cd main/
      $ ( cd benchdir/ && git status )
      $ ( cd benchdir/ && git pull --ff-only )

- (optional) examine, and perhaps delete, any test runs where tests
  have 'missing-output':

      $ cd main/
      $ grep '"output-missing"' results/*-g*-*/results.json | cut -d/ -f1-2 | sort -u

- (optional) examine (and perhaps delete) test runs with no
  results.json:

      $ cd main/
      $ ls -d results/*-g*-*/ | while read d ; do test -r $d/results.json || echo $d ; done

- (optional) examine, and perhaps delete, some test results:

  - use gime-work.sh to create a file containing, among other things,
    a list of test runs along with their commit and "interest" level
    (see below):

        $ ./benchdir/testing/web/gime-work.sh results rutdir/ 2>&1 | tee commits.tmp

  - strip the raw list of everything but test runs; also exclude the
    most recent test run (so the latest result isn't deleted):

        $ grep TESTED: commits.tmp | tail -n +2 | tee tested.tmp

  - examine, and perhaps delete, the un-interesting (false) test runs

    Un-interesting commits do not modify the C code and are not a
    merge point. These are created when HEAD, which is tested
    unconditionally, isn't that interesting.  Exclude directories
    already deleted.

        $ awk '/ false$/ { print $2 }' tested.tmp | while read d ; do test -d "$d" && echo $d ; done

  - examine, and perhaps delete, a selection of more interesting
    (true) test runs.

    More interesting commits do modify the C code but are not a merge.
    Exclude directories already deleted.

        $ awk '/ true$/ { print $2 }' tested.tmp | while read d ; do test -d "$d" && echo $d ; done | shuf | tail -n +100

- start <tt>tester.sh</tt>, see above


# After a Release (Release Engineering)

Once the release is made, the website will need an update.  Here's a
guideline:

## Roll over the web site

- in mainline, cleanup and fix testing/web et.al.

  Since the results page is going to be built from scratch, now is the
  time to make enhancements and remove fluf.

  Remember, the the website is run from the (mostly) frozen benchdir/
  so this should be save.

- in mainline, check for VM OSs that need an upgrade

  general rule is Debian is trailing edge others are leading edge

- let the website run until the release has been tested

  assuming tester.sh can see it (if not, fun times)

- upgrade/reboot machine

- move testing/ aside (to old/?) creating an empty testing/ directory

  continued below

- in benchdir/ run `./kvm demolish` (why not)

- update benchdir/ pulling in above enhancements

  SOP is to only update benchdir/ when the test scripts need an
  update.

- review benchdir/Makefile.inc.local

  Set $(WEB_BRANCH_TAG) (name subject to change) to most recent branch
  point or release.  That will be the first change tested!  After that
  changes from the tag to HEAD are tested.

  Should $(WEB_BRANCH_NAME) (name subject to change) be set?

- run `cp /dev/null nohup.out ; nohup ./benchdir/testing/web/tester.sh & tail -f nohup.out`

  tester.sh redirects its output to results/tester.log

## Archive the previous results

Arguably this should all be dropped and instead, each release captures
its test results.

- set up the variables

      o=v4.7
      n=v4.8

- create the archive directory:

      mkdir ~/${o}-${n}

- move results from previous release:

      mv ~/old/${o}-* ~/${o}-${n}

- copy result from latest release (so results are bookended with by
  releases) (tester.sh given a chance will seek out and test ${n}-0):

      cp -r ~/old/${n}-0-* ~/${o}-${n}

- now clean the archive of old logs (these should match the pattern
  OUTPUT/*):

      find ~/${o}-${n} -name 'debug.log.gz' | xargs rm -v
      find ~/${o}-${n} -name 'pluto.log' -print | xargs rm -v
      find ~/${o}-${n} -name 'iked.log' -print | xargs rm -v
      find ~/${o}-${n} -name 'charon.log' -print | xargs rm -v

- check for other files:

      find ~/${o}-${n} -name '*.log.gz' -print # delete?

  this finds some bonus stuff in OUTPUT which should be added to
  above

- check for stray logs:

      find ~/${o}-${n} -name '.log' -print # delete?

  this finds things like kvm-check.log which should be compressed

- finally re-generate the pages in the archive:

      ( cd ~/main/script-repo/ && make WEB_SUMMARYDIR=~/${o}-${n} web-summarydir )

- and restart tester.sh

  Note the addition of ${n} to specify the commit to start from.

      cp /dev/null nohup.out ; nohup ;
      ./main/benchdir/testing/web/tester.sh ${n} &



## Rebuilding

This section is out-of-date.  It is probably easier to just delete the
problematic test results and let them be rerun.

From time-to-time the web site may require a partial or full rebuild.

For HTML (.html, .css and .js) files, the process is straight forward.
However, for the .json files, the process can be slow (and in the case
of the results, a dedicated git repository is needed).

- create a repository for rebuilding the web site (aka scratch/)

  When re-generating the results from a test run (for instance as part
  of rebuilding the web-site after a json file format change), this
  repository is "git reset --hard" to the original commit used to
  generate those results.

  For instance, to set up main/scratch/:

      $ cd main/
      $ git clone https://github.com/libreswan/libreswan.git scratch/

- `make web [WEB_SCRATCH_REPODIR=.../main/scratch]`

  Update the web site.

  If WEB_SCRATCH_REPODIR is specified, then the result.json files in
  the test run sub-directories under $(WEB_SUMMARYDIR) are also
  updated.

- `make web-results-html`

  Update any out-of-date HTML (.html, .css and .json) files in the
  results sub-directories.

  Touching the source file `testing/web/results.html` will force an
  update.

- `make web-commits-json`

  Update the commits.json file which contains a table of all the
  commits.  Slow.

  Touching the script `testing/web/json-commit.sh`, which is used to
  create the files, will force an update.  So too will deleting the
  .../commits/ directory.

- `make web-results-json WEB_SCRATCH_REPODIR=.../main/scratch`

  Update the `results.json` file in each test run's sub-directory.
  Very slow.  Requires a dedicated git repository.

  Touching the script `testing/utils/kvmresults.py`, which is used to
  generate results.json, will force an update.

- `make '$(WEB_SUMMARYDIR)/<run>/results.json' WEB_SCRATCH_REPODIR=.../main/scratch`

  Update an individual test run's `results.json` file.  Slow.
  Requires a dedicated git repository.
