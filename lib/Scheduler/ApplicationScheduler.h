////////////////////////////////////////////////////////////////////////////////
/// @brief application server scheduler implementation
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2014 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
/// @author Copyright 2014, ArangoDB GmbH, Cologne, Germany
/// @author Copyright 2009-2014, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_SCHEDULER_APPLICATION_SCHEDULER_H
#define ARANGODB_SCHEDULER_APPLICATION_SCHEDULER_H 1

#include "Basics/Common.h"

#include "ApplicationServer/ApplicationFeature.h"

// -----------------------------------------------------------------------------
// --SECTION--                                              forward declarations
// -----------------------------------------------------------------------------

namespace triagens {
  namespace rest {
    class ApplicationServer;
    class Scheduler;
    class SignalTask;
    class Task;

// -----------------------------------------------------------------------------
// --SECTION--                                        class ApplicationScheduler
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief application server scheduler implementation
////////////////////////////////////////////////////////////////////////////////

    class ApplicationScheduler : public ApplicationFeature {
      private:
        ApplicationScheduler (ApplicationScheduler const&);
        ApplicationScheduler& operator= (ApplicationScheduler const&);

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor
////////////////////////////////////////////////////////////////////////////////

        explicit ApplicationScheduler (ApplicationServer*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destructor
////////////////////////////////////////////////////////////////////////////////

        ~ApplicationScheduler ();

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief allows a multi scheduler to be build
////////////////////////////////////////////////////////////////////////////////

        void allowMultiScheduler (bool value = true);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the scheduler
////////////////////////////////////////////////////////////////////////////////

        Scheduler* scheduler () const;

////////////////////////////////////////////////////////////////////////////////
/// @brief installs a signal handler
////////////////////////////////////////////////////////////////////////////////

        void installSignalHandler (SignalTask*);

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the number of used threads
////////////////////////////////////////////////////////////////////////////////

        size_t numberOfThreads ();

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the processor affinity
////////////////////////////////////////////////////////////////////////////////

        void setProcessorAffinity (const std::vector<size_t>& cores);

////////////////////////////////////////////////////////////////////////////////
/// @brief disables CTRL-C handling (because taken over by console input)
////////////////////////////////////////////////////////////////////////////////

        void disableControlCHandler ();

// -----------------------------------------------------------------------------
// --SECTION--                                        ApplicationFeature methods
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        void setupOptions (std::map<std::string, basics::ProgramOptionsDescription>&);

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        bool parsePhase1 (basics::ProgramOptions&);

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        bool parsePhase2 (basics::ProgramOptions&);

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        bool prepare ();

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        bool start ();

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        bool open ();

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

        void stop ();

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief builds the scheduler
////////////////////////////////////////////////////////////////////////////////

        void buildScheduler ();

////////////////////////////////////////////////////////////////////////////////
/// @brief builds the scheduler reporter
////////////////////////////////////////////////////////////////////////////////

        void buildSchedulerReporter ();

////////////////////////////////////////////////////////////////////////////////
/// @brief quits on control-c signal
////////////////////////////////////////////////////////////////////////////////

        void buildControlCHandler ();

////////////////////////////////////////////////////////////////////////////////
/// @brief adjusts the file descriptor limits
////////////////////////////////////////////////////////////////////////////////

        void adjustFileDescriptors ();

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief application server
////////////////////////////////////////////////////////////////////////////////

        ApplicationServer* _applicationServer;

////////////////////////////////////////////////////////////////////////////////
/// @brief scheduler
////////////////////////////////////////////////////////////////////////////////

        Scheduler* _scheduler;

////////////////////////////////////////////////////////////////////////////////
/// @brief task list
////////////////////////////////////////////////////////////////////////////////

        std::vector<Task*> _tasks;

////////////////////////////////////////////////////////////////////////////////
/// @brief interval for reports
////////////////////////////////////////////////////////////////////////////////

        double _reportInterval;

////////////////////////////////////////////////////////////////////////////////
/// @brief is a multi-threaded scheduler allowed
////////////////////////////////////////////////////////////////////////////////

        bool _multiSchedulerAllowed;

////////////////////////////////////////////////////////////////////////////////
/// @brief number of scheduler threads
/// @startDocuBlock schedulerThreads
/// `--scheduler.threads arg`
///
/// An integer argument which sets the number of threads to use in the IO
/// scheduler. The default is 1.
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

        uint32_t _nrSchedulerThreads;

////////////////////////////////////////////////////////////////////////////////
/// @brief scheduler backend
/// @startDocuBlock schedulerBackend
/// `--scheduler.backend arg`
///
/// The I/O method used by the event handler. The default (if this option is
/// not specified) is to try all recommended backends. This is platform
/// specific. See libev for further details and the meaning of select, poll
/// and epoll.
/// @endDocuBlock
////////////////////////////////////////////////////////////////////////////////

        uint32_t _backend;

////////////////////////////////////////////////////////////////////////////////
/// @brief minimum number of file descriptors
////////////////////////////////////////////////////////////////////////////////

        uint32_t _descriptorMinimum;

////////////////////////////////////////////////////////////////////////////////
/// @brief disables CTRL-C handling (because taken over by console input)
////////////////////////////////////////////////////////////////////////////////

        bool _disableControlCHandler;
    };
  }
}

#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
