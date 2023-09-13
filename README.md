
# Caspian Sandbox
Caspian sandbox is my humble attempt at building an MVP of an automateed malware analysis sandbox. For now, it is aimed at Windows 10 x64 samples exclusively. This repo contains Windows Kernel Drivers needed to perform the core functionalities of the sandbox.
# Installation Notes
- You should be aware that both drivers are using hardcoded serial port object names for communication. So, before installing, make sure you edit those object names / symlinks accordingly.
- The logger is writing to a serial port so make sure that it is linked to some file as output.
- The agent is based upon the Inverted Call Model. It has a user-mode component that sends an IOCTL as a notification mechanism and waits for the response to run the sample written to malware.exe. I didn't include in this repo because it is a mess but It could be easily coded.
# Nota Bene
This was done as an "ambitious" experiment as my first Windows kernel programming project. As such :
- This project is barely functional.
- It could probably be more optimized.
- Some bad practices and spaghetti code could surely be found.
But hey, it works right ? ;)