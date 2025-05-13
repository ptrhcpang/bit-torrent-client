# A Bit Torrent client

## 0. Introduction 

0.1 Bit torrent is a communication protocol that allows users to download large files quickly by:

1/ breaking up a file into *pieces* (further broken into blocks)

2/ allowing multiple downloaders (*peers*) to download pieces from one another

0.2 This .c compiles into a Bit Torrent client that currently allows users to download files piece by piece from possibly multiple peers using the Bit Torrent protocol. Further development will allow downloading of an entire file asynchronously from multiple peers. There is currently no choking algorithm implemented. I intend to implement one in the next iteration. I also intend to break up this .c file into a more managable folder of different files. Currently, sections of code are marked out in comments that follow the general workflow of a Bit Torrent exchange (see [Section 2](Workflow) below).

0.3 The Bit Torrent protocol is specified at this BitTorrent.org page: https://www.bittorrent.org/beps/bep_0003.html. The remaining sections of this description file summarises some of that information.

0.4 See [Section 3](Manual) below for instructions/manual. Epoll is used to handle asynchronous requests, which means any executable compiled from this .c file runs best on Linux machines.

## 1. Torrent file

### 1.1 File contents

A downloader starts with a torrent file (say, sample.torrent).

This torrent file contains: 

1/ a tracker URL to which a request can be made to find out which other downloaders are trying to download the file and announce the downloader's intention to join them

2/ the file length in bytes and a 20-character SHA1 checksum for the file

3/ the length a piece in bytes (the same for each piece except the last one, which is file length modulo piece length)

4/ the 20-character SHA1 checksum of each piece

### 1.2 Bencode file format

Torrent files and files transmitted using the Bit Torrent protocol use the Bencode file format, which is a form of encoding for four data types: integers, (byte) strings, lists, and dictionaries. From the Bit Torrent protocol page (): 

    1/ Strings are length-prefixed base ten followed by a colon and the string. For example 4:spam corresponds to 'spam'.
    
    2/ Integers are represented by an 'i' followed by the number in base 10 followed by an 'e'. For example i3e corresponds to 3 and i-3e corresponds to -3. Integers have no size limitation. i-0e is invalid. All encodings with a leading zero, such as i03e, are invalid, other than i0e, which of course corresponds to 0.
    
    3/ Lists are encoded as an 'l' followed by their elements (also bencoded) followed by an 'e'. For example l4:spam4:eggse corresponds to ['spam', 'eggs'].
    
    4/ Dictionaries are encoded as a 'd' followed by a list of alternating keys and their corresponding values followed by an 'e'. For example, d3:cow3:moo4:spam4:eggse corresponds to {'cow': 'moo', 'spam': 'eggs'} and d4:spaml1:a1:bee corresponds to {'spam': ['a', 'b']}. Keys must be strings and appear in sorted order (sorted as raw strings, not alphanumerics).

### 1.3 Blocks 

A file is broken into pieces of length that are typically multiples of 2^14 = 0x4000 bytes, along with the final piece, which length is the remainder. 

This is because each piece is usually transmitted in blocks, which are typically of size 2^14 bytes, excepting again, the final block of the final piece.

Therefore each block has an offset of a mutiple of 2^14 within its piece.

## 2. Workflow

### 2.1 Announce

As mentioned in [Section 1.1](File-contents) above, the downloader first sends a GET request to to the tracker URL to get the ip address and available port numbers of all peers. This GET request requires the downloader to provide a 20-character long peer id. A common peer id is 0011223344556678899. But a connection can be refused if the downloader's id clashes with the peer id of another downloading peer. There is an official peer id convention: https://www.bittorrent.org/beps/bep_0020.html. In this .c file, the peer id is pseudo-randomly generated.

### 2.2 Handshake

The user then establishes a TCP connection with one or more peer by exchanging a 68-byte long protocol message:


    1/ length of the protocol string (BitTorrent protocol) which is 19 (1 byte)
  
    2/ the string "BitTorrent protocol" (19 bytes) eight reserved bytes, which are all set to zero (8 bytes)
    
    3/ SHA1 hash of the .torrent file itself (20 bytes)
    
    4/ the downloader's own peer ID (20 bytes) (which does not have to be the same as the GET request URL peer id)

The peer responds with its own peer id, and bitfield indicating the pieces they have.

### 2.3 Peer messages 

All peer messages are of the following format:

    1/ message length (4 bytes)
    
    2/ message id (1 byte)
    
    3/ payload (variable)


There are nine message types (message id - name):
   
    0 - choke  
    1 - unchoke      
    2 - interested      
    3 - not interested      
    4 - have      
    5 - bitfield  
    6 - request      
    7 - piece      
    8 - cancel


They have the following payloads:

0/ (empty) This message tells downloader that peer will not allow requests at the moment. 

1/ (empty) This message tells downloader that peer will receive a request. 

2/ (empty) This message tells peer that downloader is interested in sending a request. 

3/ (empty) 

4/ piece index (1 byte) which the downloader has and checked against the checksum in the .torrent file 

5/ A binary string of length equal to the number of pieces into which the file is broken, with 1 in places where the peer has the piece and 0 where they do not. The noughth piece is in the left-most place. 

6/ index (4 bytes): index of piece requested; begin (4 bytes): offset of a block within the piece 

7/ index (4 bytes): index of piece requested; begin (4 bytes): offset of a block within the piece; block (<= 2^14 bytes): content of the block of the piece 

8/ same as request (id = 6). Multiple peers may receive the same request. This message tells all other peers that they do not have to send a piece once it has arrived from one of the peers. 


### 2.4 Remainder of workflow

The workflow of a transmission under the Bit Torrent protocol is then:

    1/ make handshake, get peer id

    2/ wait for bitfield
    
    3/ send interest
    
    4/ wait for unchoke
    
    5/ send requests for blocks
    
    6/ assemble piece and check against checksum
    
    7/ send have
    

### 2.5 Endgame

No endgame has been implemented yet.

## 3. Manual

This .c file has to be compiled with -lcurl -lcrypto flags. I included a shell script called yb1.sh.

There are currently five possible commands:

### 3.1 `./yb1.sh decode <string>`

This decodes properly b-encoded data.

### 3.2 `./yb1.sh info <filename.torrent>`

This prints out the contents of a torrent file. (See [Section 1.1](File-contents) above.)

### 3.3 `./yb1.sh peers <filename.torrent>`

This sends a GET request to the tracker URL and prints out the ip addresses of all peers.

### 3.4 `./yb1.sh handshake <filename.torrent> <peer_ip>:<peer_port>`

This establishes a TCP handshake with a peer at <peer_ip>:<peer_port> downloading the file specified in <filename.torrent>. The peer_ip is a string "ddd.ddd.ddd.ddd", where "ddd" is a string giving an integer between 0 and 255 inclusive, in decimal.

### 3.5 `./yb1.sh download_piece -o <save_path> <filename.torrent> <piece index>`

This downloads the piece of index <piece_index> of the file specified in <filename.torrent> and saves it at <save_path>.

### 3.6 `./yb1.sh download -o <save_path> <filename.torrent>`

This command is not fully implemented. This does download an entire file and saves to <save_path>, but the download is not asynchronous and simultaneous. Pieces are only downloaded one after another using `download-piece` and assembled.

## 4. Licence

This repository is distributed under the MIT licence.

