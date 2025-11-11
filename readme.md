Standalone C-Based E-Voting System

This project is a complete, self-contained e-voting application built entirely in C. It runs as its own standalone web server, eliminating the need for any external server software like Apache or Nginx. It is designed to be lightweight, portable, and easy to use for small-scale elections (e.g., clubs, classes, or teams).

Features (Current Status)

This application is fully functional and includes the following features:

Standalone Server: The entire application is a single executable. It uses the libmicrohttpd library to listen for and handle HTTP requests directly.

Voter Verification: Checks a voters.txt file to ensure that only registered individuals (matching Aadhar and Name) can cast a vote.

Duplicate Vote Prevention: Maintains a voted.txt file to block any voter from casting more than one ballot.

Visual Voting Interface: The voting page dynamically loads candidate names and photos from the candidates.txt file, providing a user-friendly experience.

Live Graphical Results: The admin panel features a dynamically generated SVG bar chart that displays the election results in real-time.

Password-Protected Admin Panel: The results page is secured with a hardcoded password (admin123) to prevent unauthorized access.

Simple Text File Database: All data (candidates, voters, votes) is stored in simple, human-readable .txt files, making the system transparent and easy to manage.

How to Compile and Run on Linux

Here are the step-by-step instructions to get the server running on a Linux machine.

1. Prerequisites (Installation)

You need a C compiler (gcc) and the libmicrohttpd development library.

On Arch Linux:

sudo pacman -S gcc libmicrohttpd


On Debian/Ubuntu-based systems:

sudo apt update
sudo apt install gcc libmicrohttpd-dev


2. Prepare Data Files

Before running the server, you must create the data files it needs to read.

candidates.txt

This file stores the list of candidates.

Format: ID,Name,ImageURL (No spaces around commas)

Example:

1,Candidate One,[https://i.imghippo.com/files/nqtQ9035nRM.jpeg](https://i.imghippo.com/files/nqtQ9035nRM.jpeg)
2,Candidate Two,[https://i.imghippo.com/files/another-link.jpg](https://i.imghippo.com/files/another-link.jpg)


Important: The ImageURL must be a direct link to a .jpg, .png, or .jpeg file. Links to image-hosting pages will not work.

voters.txt

This file is the official list of eligible voters.

Format: Aadhar,Name (No spaces around commas)

Example:

123456789012,First Voter
987654321098,Second Voter


The server will automatically create voted.txt and votes.txt if they don't exist.

3. Compile the Server

With the server.c file and data files in your project directory, run the following gcc command:

**gcc server.c -o server -lmicrohttpd**


This command compiles your code (server.c), links it with the libmicrohttpd library, and creates a single executable file named server.

4. Run the Server

Now, simply execute the program you just built:

**./server**


If successful, your terminal will display:

Server is running on http://localhost:8080
Press Enter to quit...


5. Access the Voting Portal

Your server is now live!

To Vote: Open your web browser and go to http://localhost:8080

To View Results: On the main page, enter the admin password (admin123) and click "View Results".

File Structure

.
├── server            (The executable file you create)
├── server.c          (The C source code for the server)
├── candidates.txt    (List of candidates and their image URLs)
├── voters.txt        (List of eligible voters)
├── voted.txt         (Automatically created to track who has voted)
└── votes.txt         (Automatically created to store the cast votes)
