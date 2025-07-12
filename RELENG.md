# Before the release

- review milestone

# During the Release

- Bump mk/version.mk

- Bump version and date in packaging/ files
  - debian changelog and rpm spec files

- Update check-03-vendoridcheck?
  it dumps current version's Vendor ID!

- Add release date to CHANGES

- git tag -s vx.y with commit message containing last CHANGES hunk

- git archive --format=tgz --prefix=libreswan-x.y/ HEAD > libreswan-x.y.tar.gz

- gpg -ba libreswan-x.y.tar.gz

- Before publishing, test using local build, mock build, fedora
  scratch build

- Upload CHANGES tar.gz and tar.gz.asc to
  nl.libreswan.org:/srv/www/download.libreswan.org

- Wait 15 mins for fi.libreswan.org sync

- Upload tar.gz and tar.gz.asc to github

- push tag to vault:
    git push origin tag vx.y

- push tag to github:
    ssh build@vaul.libreswan.org cd  /srv/src/libreswan.git/
    git push --follow-tags github vx.y

- push commits to github (or wait 15 mins):
    ssh build@vaul.libreswan.org ./bin/github-push.sh

# After the release

- Start new section in CHANGES with x.y+1 (unreleased)

- Post to announce@libreswan.org

  (causes mail approval msgs for swan and dev as well)

- twitter: announce using libreswan account [no longer done]

## Update GITHUB

- create a milestone for the next release

- close this release's milestone

  (it has nothing outstanding, right?!?)

- create a bug label with magic colouring

  v5.3, for instance, has the colour #0503ff, get it?

- add replease to GITHUB

  see (https://github.com/libreswan/libreswan/releases)[releases]

## Update Distros

- Build fedora release

- update pkgsrc/wip/libreswan-5/

- future: build copr releases for Centos Stream

## rebuild testing.libreswan.org

Details should be in testing.libreswan.org/README.md under "After a
Release" but a quick summary is: reboot; update/upgrade machine; save
-0- result in releases/; delete results/; re-start tester script

## Check for tasks, create bugs

- have any of the tested operating systems put out a new release?

  if so file a task against the next relese
