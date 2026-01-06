# OpenTitan OTBN

* `-global ot-otbn.logfile=<filename>` output OTBN execution message to the specified logfile. When
  _logasm_ option (see below) is not enabled, only execution termination and error messages are
  logged. `stderr` can be used to log the messages to the standard error stream instead of a file.

* `-global ot-otbn.logasm=<true|false>` dumps executed instructions on OTBN core into the _logfile_
  filename. Beware that this further slows down execution speed, which could likely result in the
  guest application on the Ibex core to time out.
