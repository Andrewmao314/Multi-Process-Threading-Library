# Multi-Process Threading Library
## Overview
This project implements a sophisticated M-to-N hybrid threading library in C that multiplexes user-level threads across multiple kernel-level threads (represented as POSIX threads). Unlike traditional N-to-1 threading models, this implementation enables true parallel execution of user threads across multiple processors.

## Key Features
Hybrid Threading Model: Implements an M-to-N threading architecture where M user threads are multiplexed across N kernel threads
Thread-Local Storage: Uses GCC's __thread extension to maintain thread-specific data and ensure isolation
Robust Synchronization: Employs POSIX mutexes and atomic operations to protect shared data structures
Context Switching: Sophisticated context management between lightweight processes (LWPs) and user threads
Signal Handling: Implements preemption through SIGVTALRM signals with proper masking during critical sections
Thread Operations: Full support for thread creation, joining, detaching, and priority-based scheduling

## Technical Implementation
The library provides comprehensive thread management capabilities including:
Thread creation, joining, and detaching
Priority-based scheduling with dynamic priority adjustment
Condition variables and mutex synchronization primitives
Automated resource cleanup through a reaper thread
Signal-based time slicing for preemptive multitasking

This project demonstrates advanced concepts in operating systems including parallel computing, thread synchronization, and resource management in a multi-processor environment.
