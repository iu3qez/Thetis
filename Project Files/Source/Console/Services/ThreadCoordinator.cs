//=================================================================
// ThreadCoordinator.cs
//=================================================================
// Thetis is a C# implementation of a Software Defined Radio.
// Copyright (C) 2004-2024  FlexRadio Systems, Doug Wigley, MW0LGE
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// Refactored to separate thread management from UI layer
//=================================================================

using System;
using System.Threading;

namespace Thetis.Services
{
    /// <summary>
    /// Manages the lifecycle of all background threads used in the Console application.
    /// This service encapsulates thread creation, starting, stopping, and cleanup logic,
    /// separating thread management concerns from the UI layer.
    /// </summary>
    public class ThreadCoordinator
    {
        #region Thread Definitions

        // Display and rendering threads
        private Thread _drawDisplayThread;

        // Meter update threads
        private Thread _multimeterThread;
        private Thread _rx2MeterThread;
        private Thread _multimeter2ThreadRx1;
        private Thread _multimeter2ThreadRx2;

        // Polling threads
        private Thread _pollPttThread;
        private Thread _pollCwThread;
        private Thread _pollPaPwrThread;
        private Thread _pollTxInhibitThread;
        private Thread _displayVoltsAmpsThread;
        private Thread _overloadThread;

        // SQL and VOX threads
        private Thread _sqlUpdateThread;
        private Thread _rx2SqlUpdateThread;
        private Thread _voxUpdateThread;
        private Thread _noiseGateUpdateThread;

        #endregion

        #region Public Methods - Thread Lifecycle Management

        /// <summary>
        /// Starts a thread if it's not already running.
        /// </summary>
        /// <param name="threadName">Unique identifier for the thread</param>
        /// <param name="threadStart">The method to execute in the thread</param>
        /// <param name="name">Display name for the thread</param>
        /// <param name="priority">Thread priority (default: Normal)</param>
        /// <returns>True if thread was started, false if already running</returns>
        public bool StartThread(string threadName, ThreadStart threadStart, string name, ThreadPriority priority = ThreadPriority.Normal)
        {
            Thread thread = GetThreadByName(threadName);

            if (thread == null || !thread.IsAlive)
            {
                thread = new Thread(threadStart)
                {
                    Name = name,
                    Priority = priority,
                    IsBackground = true
                };

                SetThreadByName(threadName, thread);
                thread.Start();
                return true;
            }

            return false;
        }

        /// <summary>
        /// Stops a thread gracefully with a timeout, then aborts if necessary.
        /// </summary>
        /// <param name="threadName">Unique identifier for the thread</param>
        /// <param name="timeoutMs">Timeout in milliseconds before forcing abort</param>
        /// <returns>True if thread was stopped, false if thread didn't exist</returns>
        public bool StopThread(string threadName, int timeoutMs = 500)
        {
            Thread thread = GetThreadByName(threadName);

            if (thread != null && thread.IsAlive)
            {
                try
                {
                    if (!thread.Join(timeoutMs))
                    {
                        thread.Abort();
                    }
                }
                catch (ThreadAbortException)
                {
                    // Expected when aborting thread
                }
                catch (Exception ex)
                {
                    // Log or handle unexpected exceptions
                    System.Diagnostics.Debug.WriteLine($"Error stopping thread {threadName}: {ex.Message}");
                }

                return true;
            }

            return false;
        }

        /// <summary>
        /// Checks if a specific thread is currently running.
        /// </summary>
        public bool IsThreadAlive(string threadName)
        {
            Thread thread = GetThreadByName(threadName);
            return thread != null && thread.IsAlive;
        }

        /// <summary>
        /// Stops all managed threads.
        /// </summary>
        public void StopAllThreads()
        {
            StopThread("DrawDisplay", 500);
            StopThread("Multimeter", GetMeterDelayTimeout());
            StopThread("RX2Meter", GetMeterDelayTimeout());
            StopThread("Multimeter2RX1", 100);
            StopThread("Multimeter2RX2", 100);
            StopThread("SQLUpdate", 500);
            StopThread("RX2SQLUpdate", 500);
            StopThread("NoiseGateUpdate", 500);
            StopThread("VOXUpdate", 500);
            StopThread("PollPTT", 500);
            StopThread("PollCW", 500);
            StopThread("PollPAPWR", 500);
            StopThread("Overload", 500);
            StopThread("PollTXInhibit", 500);
            StopThread("DisplayVoltsAmps", 650);
        }

        #endregion

        #region Private Helper Methods

        private Thread GetThreadByName(string threadName)
        {
            switch (threadName)
            {
                case "DrawDisplay": return _drawDisplayThread;
                case "Multimeter": return _multimeterThread;
                case "RX2Meter": return _rx2MeterThread;
                case "Multimeter2RX1": return _multimeter2ThreadRx1;
                case "Multimeter2RX2": return _multimeter2ThreadRx2;
                case "PollPTT": return _pollPttThread;
                case "PollCW": return _pollCwThread;
                case "PollPAPWR": return _pollPaPwrThread;
                case "PollTXInhibit": return _pollTxInhibitThread;
                case "DisplayVoltsAmps": return _displayVoltsAmpsThread;
                case "SQLUpdate": return _sqlUpdateThread;
                case "RX2SQLUpdate": return _rx2SqlUpdateThread;
                case "VOXUpdate": return _voxUpdateThread;
                case "NoiseGateUpdate": return _noiseGateUpdateThread;
                case "Overload": return _overloadThread;
                default: return null;
            }
        }

        private void SetThreadByName(string threadName, Thread thread)
        {
            switch (threadName)
            {
                case "DrawDisplay": _drawDisplayThread = thread; break;
                case "Multimeter": _multimeterThread = thread; break;
                case "RX2Meter": _rx2MeterThread = thread; break;
                case "Multimeter2RX1": _multimeter2ThreadRx1 = thread; break;
                case "Multimeter2RX2": _multimeter2ThreadRx2 = thread; break;
                case "PollPTT": _pollPttThread = thread; break;
                case "PollCW": _pollCwThread = thread; break;
                case "PollPAPWR": _pollPaPwrThread = thread; break;
                case "PollTXInhibit": _pollTxInhibitThread = thread; break;
                case "DisplayVoltsAmps": _displayVoltsAmpsThread = thread; break;
                case "SQLUpdate": _sqlUpdateThread = thread; break;
                case "RX2SQLUpdate": _rx2SqlUpdateThread = thread; break;
                case "VOXUpdate": _voxUpdateThread = thread; break;
                case "NoiseGateUpdate": _noiseGateUpdateThread = thread; break;
                case "Overload": _overloadThread = thread; break;
            }
        }

        private int GetMeterDelayTimeout()
        {
            // This should be configurable from the console, for now default to 550ms
            return 550;
        }

        #endregion
    }
}
