#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <stdbool.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>


//** SECTION 1 */
//functions for decoding bencoded information

/*listStruct was designed to hold 
  decoded lists and dictionaries.
  But it is also used to open torrent
  files and hold their content, with 
  bLength set to zero, or to receive 
  blocks from peers. 
  It can be used in place of 
  MemStruct in the same way.
*/
struct listStruct{
    int bLength;//length of encoded dict/list
    long dLength;//length of decoded list/dict
    unsigned char* decoded_str;
};


/*listCounter is used to store 
  stats about lists and dictionaries.*/
struct listCounter{
    int stepper;    //length of encoded value
    int itemCount;  //items in list/dict
    int colon_gap;  //for strings only: log_10(length) + 1
    int stringCount;//how many strings in list/dict and sub list/dicts
};



/*bencoded strings are of four types:
    1/ integers,
    2/ (byte) strings,
    3/ lists,
    4/ dictionaries.
The decode_bencode() function below
handles all these by referring to 
the three handler functions, one for 
each of types 1 and 2, and types 3 
and 4 are handled together.*/


bool is_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}

//steps through dictionaries or other b-encoded types to return stats
struct listCounter gStepper(const char* encoded_str, bool listflag){
    struct listCounter result;
    int stepper = 0;
    if(listflag){
        stepper = 1;
    }
    char* next_colon;
    struct listCounter inlistCount;
    int itemCount = 0;
    result.stringCount = 0;
    result.colon_gap = 0;

    while(true){ 
        if(is_digit(encoded_str[stepper])){
            next_colon = strchr(encoded_str + stepper,':');
            stepper = (next_colon - encoded_str) + atoi(encoded_str + stepper) + 1;
            itemCount +=1;
            result.stringCount += 1;
            if(!listflag){
                result.stepper = stepper;
                result.itemCount = itemCount;
                result.colon_gap = next_colon - encoded_str;
                return result;
            }
        }else{
            switch(encoded_str[stepper]){
                case 'i'://integer
                    next_colon = strchr(encoded_str + stepper,'e');
                    stepper = (next_colon - encoded_str) + 1;
                    itemCount +=1;
                    if(!listflag){
                        result.stepper = stepper;
                        result.itemCount = itemCount;
                        return result;
                    }
                    break;
                case 'l'://list
                    list_case2:
                    inlistCount = gStepper(encoded_str+ stepper,true);
                    stepper += 1 + inlistCount.stepper;
                    result.stringCount += inlistCount.stringCount;
                    itemCount +=1;
                    break;
                case 'd'://dictionary
                    goto list_case2;
                case 'e':
                    //stepper += 1;
                    result.stepper = stepper;
                    result.itemCount = itemCount;
                    return result;
                default:
                    fprintf(stderr,"Invalid bencoding: %s\n",encoded_str);
                    exit(1);
            }

        }
        //end of while looop
    }

}

struct listStruct fileOpener(const char* file_name){

    struct listStruct fileStats;

    FILE *fptr;
    fptr = fopen(file_name,"r");
    if(fptr == NULL){
        fprintf(stderr, "Cannot open file: %s\n", file_name);
        exit(1);
    }
    
    long fileLength = 0;
    fseek(fptr, 0, SEEK_END);
    fileLength = ftell(fptr);
    //printf("file length: %ld\n", fileLength);
    rewind(fptr);
    
    unsigned char* read_str = (unsigned char*)malloc(fileLength + 1);
    memset(read_str,0,fileLength + 1);

    long readLength = fread(read_str,1,fileLength,fptr);
    if (readLength != fileLength){
        fprintf(stderr,"File read error\n");
        fclose(fptr);
        exit(1);
    }

    fclose(fptr);

    fileStats.bLength = 0;
    fileStats.dLength = fileLength;
    fileStats.decoded_str = read_str;
    return fileStats;
}


//decodes integers
unsigned char* int_handler(const unsigned char* bencoded_value) {
       const unsigned char* numend_index = strchr(bencoded_value,'e');
        if(numend_index != NULL ){
            int length = numend_index - bencoded_value - 1;
            const unsigned char* start = bencoded_value + 1;
            unsigned char* decoded_str = (unsigned char*)malloc(length + 1);
            strncpy(decoded_str,start, length);
            decoded_str[length] = '\0';
            //test if string is actually all numeric between i and e:
            for(int i = 0; i < length; i++){
                if(!(is_digit(decoded_str[i])||(i==0 && decoded_str[0]=='-'))){
                    //cleaup before exit necessary?
                    free(decoded_str);
                    decoded_str = NULL;
                    fprintf(stderr, "Invalid encoded value: %s\n", bencoded_value);
                    exit(1);
                }
            }
            //end test
            return decoded_str;
        }else{
            fprintf(stderr, "Invalid encoded value: %s\n", bencoded_value);
            exit(1);
        }           
}

//decodes strings
unsigned char* str_handler(const unsigned char* bencoded_value) {
        int length = atoi(bencoded_value);
        unsigned char* colon_index = strchr(bencoded_value, ':');
        if (colon_index != NULL) {
            unsigned char* start = colon_index + 1;
            unsigned char* decoded_str = (unsigned char*)malloc(length + 3);
            decoded_str[0] = '\"';
            memcpy(decoded_str + 1, start, length);
            decoded_str[length + 1] = '\"';
            decoded_str[length + 2] = '\0';
            return decoded_str;
        } else {
            fprintf(stderr, "Invalid encoded value: %s\n", bencoded_value);
            exit(1);
        }
}


//decodes lists and dictionaries
struct listStruct list_handler(const unsigned char* bencoded_value, const unsigned char listdict){
    if(listdict != 'l' && listdict != 'd'){
        fprintf(stderr, "Invalid encoded value: %s\n", bencoded_value);
        exit(1);
    }

    struct listCounter listStats = gStepper(bencoded_value,true);

    struct listStruct intern_listStruct;
    int b_stepper = 0;//stepper on encoded string
    int d_stepper = 0;//stepper on decoded string
    int colon_no = listStats.stringCount;
    int colon_gap = 0;
    int tlength = 0;//length of newly decoded substring
    char in_listdict = 'l';//listdict for inner list/dict
    unsigned char* tdecoded_str = NULL; //newly decoded segment/item
    bool comma_flag = false;//for dict: for interchangning ',' and ':' --- raise if next is ','
    bool key_flag = false;//raise if key is not string
    bool d_flag = false;//raise if next item is a dict

   
    unsigned char* decoded_str = (unsigned char*)malloc(listStats.stepper + colon_no + 3);

    //put in opening brackets:
    if(listdict == 'l'){
        decoded_str[0] = '[';
    }else{
        decoded_str[0] = '{';
    }
    b_stepper += 1; d_stepper += 1;

    while(b_stepper < listStats.stepper + 1){
        //this loop consists of two parts:
        //1.decode substring
        //2.copy decoded substring onto decoded_str

        //decode substring
        list_exit:
        if(bencoded_value[b_stepper] == 'e'){
            //if start of next item is e, end of list reached
            break;
        }
        if(is_digit(bencoded_value[b_stepper])){
            colon_gap = (unsigned char*)strchr(bencoded_value + b_stepper, ':') - bencoded_value -  b_stepper;
            tdecoded_str = str_handler(bencoded_value + b_stepper);//copy next item
            tlength = atoi(bencoded_value + b_stepper) + 2;
            //move bencoded_value + b_stepper along on bencoded_value
            //string in bencode <length>:<string> 
            //-2:quotations, colon_gap:<length>, +1
            b_stepper += tlength - 2 + colon_gap + 1;
        }else{
            switch(bencoded_value[b_stepper]){
                case 'i':
                    tdecoded_str = int_handler(bencoded_value + b_stepper);//copy next item
                    tlength = strlen(tdecoded_str); //find length of next item 
                    //move b_stepper along on bencoded_value
                    b_stepper += tlength + 2; //integers in bencode i<number>e
                    if(!comma_flag&& listdict == 'd'){
                    //raise flag if this is a dict but next item (int) is a key
                        key_flag = true;
                    }
                    break;
                case 'l':
                    d_flag = false;//list not dict
                    case_l:
                    if(!comma_flag&& listdict == 'd'){
                        key_flag = true;
                    }
                    if(d_flag){in_listdict = 'd';}//dict not list
                    intern_listStruct = list_handler(bencoded_value + b_stepper,in_listdict);//copy next item
                    tdecoded_str = intern_listStruct.decoded_str;
                    tlength = intern_listStruct.dLength;
                    b_stepper += intern_listStruct.bLength;
                    //move b_stepper along on bencoded_value
                    d_flag = false;
                    break;
                case 'd':
                    d_flag = true;
                    goto case_l;
                    //break;
                case 'e':
                    goto list_exit;
                    //break;
                default:
                    fprintf(stderr, "Invalid encoded value: %s\n", bencoded_value);
                    exit(1);
            }
        }

        //copy decoded substring onto decoded_str:
        memcpy(decoded_str + d_stepper,tdecoded_str,tlength);
        free(tdecoded_str);
        tdecoded_str = NULL;

        //move decoded_str along using d_stepper
        d_stepper += tlength + 1;//+1:comma (or colon)
        if(comma_flag || listdict == 'l'){
            decoded_str[d_stepper - 1] = ',';
            comma_flag = false;
        }else{
            decoded_str[d_stepper - 1] = ':';
            comma_flag = true;
        }
           
        //end of while loop
    }

    if(d_stepper == 1){
        d_stepper += 1;//if list is empty:
    }

    if(listdict == 'l'){
        decoded_str[d_stepper - 1] = ']';//replace final comma
    }else{
        decoded_str[d_stepper - 1] = '}';//replace final comma
    }

    decoded_str[d_stepper] = '\0';

    if(comma_flag){
        fprintf(stderr, "Invalid decoded value: %s\n", decoded_str);
        printf("Final key not paired with value.\n");
        exit(1);
    }
    if(key_flag){
        fprintf(stderr, "Invalid decoded value: %s\n", decoded_str);
        printf("List and dictionaries cannot be keys.\n");
        exit(1);
    }

    struct listStruct dc_listStruct = {listStats.stepper + 1,d_stepper, decoded_str};
    return dc_listStruct;

}


unsigned char* decode_bencode(const unsigned char* bencoded_value) {
    
    if (is_digit(bencoded_value[0])) {
        return str_handler(bencoded_value);
    }

    if (bencoded_value[0] == 'i'){
        return int_handler(bencoded_value);
    }

    if (bencoded_value[0] == 'l'){
        struct listStruct tempStruct = list_handler(bencoded_value,'l');
        return tempStruct.decoded_str;
    }
        
    if (bencoded_value[0] == 'd'){
        struct listStruct tempStruct = list_handler(bencoded_value,'d');
        return tempStruct.decoded_str;
    }
    
    else {
       fprintf(stderr, "Invalid encoded value: %s\n", bencoded_value);
       exit(1);
   }
}



//** SECTION 2 */
//functions for extracting information from torrent files
//openssl/sha.h used here

struct torInfo{
    unsigned char* announce;
    int announce_length;
    // unsigned char* name;
    long file_length;
    long piece_length;
    unsigned char* piece_hashes;
    int hashes_length;
    unsigned char* info_hash;
};

//generates pseudo-random 9-digit numeric string for peer_id
char* randnum_generator(){

    size_t t = (size_t)time(NULL);
    int j = (int)((t*t)%2147483648);
    j  = j%1000000000;
    char* randnum = (char*)malloc(10);
    memset(randnum,0,10);
    memset(randnum,48,9);
    char anynum[10];
    memset(anynum,0,10);
    sprintf(anynum,"%d",j);
    memcpy(randnum,anynum,strlen(anynum));

    return randnum;
}

// function for producing hash key 
unsigned char* findHash(const unsigned char* encoded_str){
    
    const unsigned char* substring = strstr(encoded_str,"4:info") + 6;

    struct listCounter listStats = gStepper(substring, true);
    int dictLength = gStepper(substring, true).stepper + 1;
    
    unsigned char buffer[dictLength];

    //memcpy
    for(int i = 0; i < dictLength; i++){
        buffer[i] = substring[i]; 
    }
   
    unsigned char* hash = (unsigned char*)malloc(SHA_DIGEST_LENGTH);
    memset(hash, 0x0, SHA_DIGEST_LENGTH); 
    SHA1(buffer, dictLength, hash);

    return hash;

}


// //function for finding values in the info dictionary
struct torInfo findValues(const unsigned char* encoded_str){
    const char* keyName;
    int stepper = 0;
    unsigned char* decoded_str = NULL;
    const unsigned char* substring = NULL;

    struct torInfo torrent_info;

    keyName = "8:announce";
    stepper = strlen(keyName);
    substring = strstr(encoded_str,keyName);
    int announce_length = atoi(substring + stepper);
    unsigned char* announce = (unsigned char*)malloc(announce_length + 1);
    decoded_str = str_handler(substring + stepper) + 1;
    decoded_str[announce_length] = '\0';//becaus str_handler comes with quotations
    memcpy(announce,decoded_str,announce_length + 1);
    free(decoded_str - 1);
    decoded_str = NULL;

    
    keyName = "6:length";
    stepper = strlen(keyName);
    substring = strstr(encoded_str,keyName);
    long file_length = atoi(substring + stepper + 1);


    // keyName = "4:name";
    // stepper = strlen(keyName);
    // substring = strstr(encoded_str,keyName);
    // int name_length = atoi(substring + stepper);
    // unsigned char* name = (unsigned char*)malloc(name_length);
    // decoded_str = str_handler(substring + stepper) + 1;//need allocated mem to do this
    // decoded_str[announce_length] = '\0';
    // memcpy(name,decoded_str,name_length);
    // free(decoded_str - 1);
    // decoded_str = NULL;
    
 
    unsigned char* info_hash = findHash(encoded_str);

    keyName = "12:piece length";
    stepper = strlen(keyName);
    substring = strstr(encoded_str,keyName);
    long piece_length = atoi(substring + stepper + 1);


    keyName = "6:pieces";
    stepper = strlen(keyName);
    substring = strstr(encoded_str,keyName);
    int hashes_length = atoi(substring + stepper);
    decoded_str = str_handler(substring + stepper) + 1;//need allocated mem to do this
    unsigned char* piece_hashes = (unsigned char*)malloc(hashes_length);
    memcpy(piece_hashes,decoded_str,hashes_length);
    free(decoded_str - 1);
    decoded_str = NULL;
    

    torrent_info.announce = announce;
    torrent_info.announce_length = announce_length;
    torrent_info.file_length = file_length;
    // torrent_info.name = name;
    torrent_info.info_hash = info_hash;
    torrent_info.piece_length = piece_length;
    torrent_info.piece_hashes = piece_hashes;
    torrent_info.hashes_length = hashes_length;
    
    return torrent_info;
}

void printTorInfo(struct torInfo torrent_info){

    printf("Tracker URL: %s\n",torrent_info.announce);
    printf("Length: %ld\n",torrent_info.file_length);
    // printf("Name: %s\n", torrent_info.name);
    printf("Info Hash: ");
    for(int i = 0; i<20; i++){
        printf("%02x",torrent_info.info_hash[i]);
    }
    printf("\n");

    printf("Piece Length: %ld\n",torrent_info.piece_length);
    printf("Piece Hashes:\n");
    int hashNL = 0;
    int dcLength = torrent_info.hashes_length;
    for(int i = 0; i < dcLength ;i++){
        hashNL += 1;
        printf("%02x",torrent_info.piece_hashes[i]);
        if(hashNL==20){
            printf("\n");
            hashNL = 0;
        }
    }

}

/** SECTION 3 */
///
///peers discovery and handling functions
//curl/curl.h used here

//MemStruct can be replaced by listStruct with bLength = 0.
struct MemStruct{
    unsigned char* memory;
    size_t mem_size;
};

//function for storing response in MemStruct
static size_t mem_Callback(void* contents, size_t mem_size, size_t mem_n, void* user_ptr){
    size_t realise = mem_size * mem_n;
    struct MemStruct *dream = (struct MemStruct *)user_ptr;

    unsigned char* dream_ptr = realloc(dream->memory, dream->mem_size + realise + 1);

    if(!dream_ptr){ printf("MemStruct realloc unsuccessful.\n"); return 0;}

    dream->memory = dream_ptr;
    memcpy(&(dream->memory[dream->mem_size]), contents, realise);
    dream->mem_size += realise;
    dream->memory[dream->mem_size] = 0;

    return realise;
}



//construct request URL
char* peers(struct torInfo torrent_info,struct peerStats* peer_ptr){
    
    //construct parts of URL
    int url_length = 0;

    int dcLength = torrent_info.announce_length;
    unsigned char decoded_url[dcLength + 1];
    memset(decoded_url,0,dcLength + 1);
    memcpy(decoded_url,torrent_info.announce,dcLength);
    decoded_url[dcLength] = '?';
    url_length += dcLength + 1;

    char esc_infohash[70];
    memset(esc_infohash,0,70);
    memcpy(esc_infohash,"info_hash=",10);
    for(int i = 0; i<20; i++){
        esc_infohash[10 + 3*i] = '%';
        sprintf(esc_infohash + 10 + 3*i + 1,"%02x",torrent_info.info_hash[i]);
    }
    url_length += 70;


    char* rando = randnum_generator();
    
    unsigned char peer_id[] = "&peer_id=42112726455463728130";
    for(int k = 0; k < 9; k++){
        peer_id[10 + k] = rando[k];
        peer_id[19 + k] = rando[k];
    }
    //printf("peer_id part of URL: %s\n", peer_id);

    // unsigned char* peer_id = "&peer_id=42112726455463728130";
    url_length += strlen(peer_id);

    unsigned char* port = "&port=6881";
    url_length += strlen(port);

    // char* i_uploaded;
    // if(peer_ptr == NULL){
    //     i_uploaded = 0;
    // }
    unsigned char* uploaded = "&uploaded=0";
    
    // char* i_downloaded;
    // if(peer_ptr == NULL){
    //     i_downloaded = 0;
    // }
    unsigned char* downloaded = "&downloaded=0";

    url_length += strlen(uploaded) + strlen(downloaded);

    long i_left = torrent_info.file_length;
    unsigned char left[27];
    memset(left,0,27);
    memcpy(left,"&left=",6);
    sprintf(left + 6,"%ld",torrent_info.file_length);
    url_length += strlen(left);

    unsigned char compact[10];
    memcpy(compact,"&compact=1",10);
    url_length += 10;

   
    //assemble full URL
    int stepper = 0;
    char* FULL_URL = (char*)malloc(url_length + 1);
    memset(FULL_URL,0,url_length+1);

    //can be refactored with memcat instead of stepper
    memcpy(FULL_URL,decoded_url,dcLength + 1);
    stepper += dcLength + 1;
    memcpy(FULL_URL + stepper,esc_infohash,70);
    stepper += 70;
    memcpy(FULL_URL + stepper,peer_id,strlen(peer_id));
    stepper += strlen(peer_id);
    memcpy(FULL_URL + stepper,port,strlen(port));
    stepper += strlen(port);
    memcpy(FULL_URL + stepper,uploaded,strlen(uploaded));
    stepper += strlen(uploaded);
    memcpy(FULL_URL + stepper,downloaded,strlen(downloaded));
    stepper += strlen(downloaded);
    memcpy(FULL_URL + stepper,left,strlen(left));
    stepper += strlen(left);
    memcpy(FULL_URL + stepper,compact,10);

    return FULL_URL;
}

//make requests and returns <ip>:<port>s of peers
struct listStruct makeGETRequest(char* FULL_URL){
    CURL *curl;
    CURLcode response;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    struct MemStruct dreamscape;
    dreamscape.memory = malloc(1);
    dreamscape.mem_size = 0;

    curl_easy_setopt(curl,CURLOPT_URL, FULL_URL);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);

    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,mem_Callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&dreamscape);

    //send request
    response = curl_easy_perform(curl);
    if(response != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n",curl_easy_strerror(response));
        exit(1);
    }
    
    //extract peer info from response 
    //and store it in a listStruct
    struct listStruct peerAddr;
    unsigned char* substring = strstr(dreamscape.memory,"5:peers");
    int dcLength = atoi(substring + 7);
    unsigned char* decoded_resp = str_handler(substring + 7) + 1;
    unsigned char* peer_info = (unsigned char*)malloc(dcLength + 1);
    memset(peer_info,0,dcLength + 1);
    memcpy(peer_info, decoded_resp,dcLength);

    peerAddr.bLength = dcLength;
    peerAddr.dLength = 0;
    peerAddr.decoded_str = peer_info;
    
    //clean up
    curl_easy_cleanup(curl);
    free(dreamscape.memory);
    dreamscape.memory = NULL;
    substring = NULL;
    free(decoded_resp - 1);
    decoded_resp = NULL;

    curl_global_cleanup();

    return peerAddr;

}

//print peers <ip>:<port> info (see ipExtract() below)
void printPeersAddr(struct listStruct peerAddr){
    unsigned char* peer_info = peerAddr.decoded_str;
    // int dcLength = peerAddr.bLength;
    
    for(int i = 0; i < peerAddr.bLength; i ++){

        if(i%6== 0){
            printf("%d",peer_info[i]);//first byte of ip address
        }else if(i%6 == 4){//two bytes for port
            printf(":%d\n", peer_info[i]*0x100 + peer_info[i + 1]);
             i = i + 1;
        }else{
            printf(".%d",peer_info[i]);//remaining three bytes of ip address
        }

    }
    
    return;
 }

///
/** SECTION 4 */
///TCP handshake

struct listStruct portExtract(const char* peer_info){

    struct listStruct SERVER_ADDR;

    int colon = strchr(peer_info,':') - peer_info;
    unsigned char* SERVER_IP = (char*)malloc(colon + 1);
    int SERVER_PORT;
    strncpy(SERVER_IP,peer_info,colon);
    SERVER_IP[colon] = '\0';
    SERVER_PORT = atoi(peer_info + colon + 1);
    // SERVER_IP is used in the function inet_pton() in tcpConnection().
    // inet_pton() takes a SERVER_IP as a character string of the form 
    // "ddd.ddd.ddd.ddd", where "ddd" is a decimal number of up to three
    // digits in the range 0 to 255.

    SERVER_ADDR.decoded_str = SERVER_IP;
    SERVER_ADDR.bLength = SERVER_PORT;
    SERVER_ADDR.dLength = 0;

    return SERVER_ADDR;

}

//makes tcp connection to server with ip (char*) and port (int)
int tcpConnection(struct listStruct SERVER_ADDR){

    char* SERVER_IP = SERVER_ADDR.decoded_str;
    int SERVER_PORT = SERVER_ADDR.bLength;
    
    int sockfd;
    struct sockaddr_in server_addr;
    int bytes_received;

    //socket making
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed.");
        exit(EXIT_FAILURE);
    }

    // Set server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported.");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Connect to server
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed.");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    return sockfd;
 
}

//generates handshake message, which consists of:

    // length of the protocol string (BitTorrent protocol) which is 19 (1 byte)
    // the string "BitTorrent protocol" (19 bytes)
    // eight reserved bytes, which are all set to zero (8 bytes)
    // sha1 infohash (20 bytes)
    // peer id (20 bytes) (does not have to be the same as the GET request URL peer_id)

char* peerID(int sockfd, const unsigned char* encoded_str){    
    
    int bytes_received;

    unsigned char tcp_msg[68];
    memset(tcp_msg,0,68);
    unsigned char* info_hash = findHash(encoded_str);
    unsigned char* buffer = (unsigned char*)malloc(68);
    memset(buffer,0,20);

    //compose bittorrent message
    tcp_msg[0] = 19;
    memcpy(tcp_msg + 1,"BitTorrent protocol",19);
    memcpy(tcp_msg + 28,info_hash,20);
    free(info_hash);
    info_hash = NULL;

    
    char* rando = randnum_generator();
    unsigned char peer_id[] = "42112726455463728130";
    for(int k = 0; k < 9; k++){
        peer_id[1 + k] = rando[k];
        peer_id[10 + k] = rando[k];
    }
    // memcpy(tcp_msg + 48, "09182736455263728190",20);
    memcpy(tcp_msg + 48, peer_id,20);


    //send bittorrent message
    send(sockfd, tcp_msg, 68, 0);

    // Receive data from server into buffer
    bytes_received = recv(sockfd, buffer, 68, 0);
    if (bytes_received < 0) {
        perror("Receive failed.");
        exit(1);
    } 
    if (bytes_received == 0) {
        printf("Server closed the connection.\n");
        exit(1);
    }

    return buffer;
}


void printPeerID(unsigned char* buffer){
    printf("Peer ID: ");
    if(buffer!=NULL){
        for(int i = 48; i < 68; i++){
            printf("%02x",buffer[i]);
        }
        printf("\n");
        return;
        }
    printf("-- no peer id received --");
        return;
}

////
/** SECTION 5 */
//functions for downloading a piece

/*peer messages format:

    1. message length (4)
    2. message id (1)
    3. payload (variable)

    message types:
    1.bitfield (recv, id = 5)
        ignore payload
    2.interested (send, id = 2)
        empty payload
    3.unchoke (recv, id = 1)
        empty payload
    4.request (send, id = 6)
        payload:
            index (4): index of piece requested
            begin (4): offset within the piece
            length = 2^14 = 0x4000 + remainder
    5.cancel (send, id = 8)
        payload:
            same as request
    6.piece (recv, id = 7)
        payload:
            index
            begin
            block of size 2^14 + remainder
    */

/*workflow:
    suppose we have:
        1.piece hash
        2.piece length
    1. make handshake, get peer id
    **
    2. wait for bitfield
    3. send interest
    **
    4. wait for unchoke
    5. send requests
    6. assemble piece and checksum


*/

#define MAX_EVENTS 64
#define BUFFER_SIZE 32768
#define MAX_PEERS 128

//tracks individual peers
struct peerStats{
	unsigned char* peer_id;
	unsigned char* ip_address;
	int port;
	int sockfd;
	
	bool handshook;
	// bool snubbed;
	
	bool is_choked;
	bool is_interested;
	bool am_choked;
	bool am_interested;
	
	size_t downloaded;	    //total bytes downloaded
	size_t uploaded;		//total bytes uploaded

	time_t last_recv;		    //timestamp of last received
	// size_t last_unchoked;	//timestamp of last unchoke
	// size_t last_sent;		//timestamp of last sent
    
    bool* have;              //piece indices peer has

	// unsigned char* bitfield;
	// int pending;
	unsigned char* recv_buffer;
	unsigned char* send_buffer;

    int recv_buffer_count;
    int send_buffer_count;
	
};

//tracks state of downloading for piece of interest
struct pieceStats{

    int piece_index;
    int piece_length;
    int block_num;              //number of blocks
    bool* blocks_have;          //blocks in possession
    unsigned char* piece_hash;  //checksum
    int block_remainder;        //size of last block
    unsigned char* buffer;      //holds downloaded payload

};


unsigned int chartoi(unsigned char* length_four){
    unsigned int total = 0;
        total += length_four[0]*0x1000000;
        total += length_four[1]*0x10000;
        total += length_four[2]*0x100;
        total += length_four[3];
    
    return total;
}


unsigned char* itochar(unsigned int length_four){
    unsigned char* blength = (unsigned char*) malloc(4);
    blength[3] = length_four&0xff;
    blength[2] = (length_four&0xff00) >>8;
    blength[1] = (length_four&0xff0000) >>16;
    blength[0] = (length_four&0xff000000) >>24;
    
    return blength;
}


//extract server address in correct format from list of peers
//(see printPeersAddr() above)
struct listStruct ipExtract(unsigned char* peer_ip){

    struct listStruct SERVER_ADDR;
    SERVER_ADDR.bLength = 0;

    char* ip_address = (char*)malloc(16);
    char ip_section[4];
    memset(ip_address,0,16);

    for(int i = 0; i < 4; i++){

        memset(ip_section,0,4);
        sprintf(ip_section,"%d",peer_ip[i]);
        strcat(ip_address,ip_section);
        if(i < 3){strcat(ip_address,".");}
        
    }

    SERVER_ADDR.decoded_str = ip_address;
    SERVER_ADDR.bLength = peer_ip[4]*0x100 + peer_ip[5];
    SERVER_ADDR.dLength = 0;

    return SERVER_ADDR;

}

//populate pieceStat fields
struct pieceStats populate_piece_stats(int piece_index, int piece_num, struct torInfo* torrent_info){

    struct pieceStats thisPiece;
    thisPiece.piece_index = piece_index;
    thisPiece.block_remainder = 0x4000;

    unsigned char* piece_hash = (unsigned char*)malloc(20);
    memcpy(piece_hash,torrent_info->piece_hashes + piece_index*20,20);
    thisPiece.piece_hash = piece_hash;

    int block_num;
    //piece_length is given by torrent file if the piece is not the final piece
    if(piece_index < piece_num - 1){
        thisPiece.piece_length = torrent_info->piece_length;
        block_num = (int)(torrent_info->piece_length/0x4000);
    }else{
        //otherwise we have to calculate the length of the final piece
        thisPiece.piece_length = (torrent_info->file_length - (piece_num - 1)*torrent_info->piece_length);
        //and we have to calculate the length of the final block of the final piece
        thisPiece.block_remainder = thisPiece.piece_length%0x4000;
        block_num = (int)(thisPiece.piece_length/0x4000);
        if(thisPiece.block_remainder != 0){
            block_num = block_num + 1;
        }
    }
    thisPiece.block_num = block_num;

    bool x = true;

    bool* blocks_have = (bool*)malloc(block_num*sizeof(x));
    memset(blocks_have, 0, block_num);
    thisPiece.blocks_have = blocks_have;

    unsigned char* piece_buffer = (unsigned char*)malloc(torrent_info->piece_length);
    memset(piece_buffer,0,torrent_info->piece_length);
    thisPiece.buffer = piece_buffer;

    return thisPiece;
}


//populate and register all peers with epoll
void register_peers(int epoll_fd, int peer_sock, int peer_port, int piece_num, struct peerStats* peer, unsigned char* peersAddr_str, unsigned char* openedFile_str){
    
    // for registration with epoll
    struct epoll_event ev;
    
    //populate peer info
    peer->sockfd = peer_sock;
    peer->port = peer_port;

    unsigned char* ip_address = (unsigned char*)malloc(4);
    memcpy(ip_address, peersAddr_str, 4);
    peer->ip_address = ip_address;

    unsigned char* peer_id = (unsigned char*)malloc(20);
    unsigned char* peer_hash = peerID(peer_sock, openedFile_str);
    memcpy(peer_id,peer_hash + 48,20);
    peer->peer_id = peer_id;

    peer->handshook = true;
    peer->downloaded = 0;
    peer->uploaded = 0;
    peer->recv_buffer = malloc(BUFFER_SIZE);
    memset(peer->recv_buffer,0,BUFFER_SIZE);
    peer->recv_buffer_count = 0;

    peer->send_buffer = malloc(BUFFER_SIZE);
    memset(peer->send_buffer,0,BUFFER_SIZE);
    
    peer->have = malloc(piece_num);
    memset(peer->have,0, piece_num);

    //set peer_sock to unblocking
    fcntl(peer_sock, F_SETFL, fcntl(peer_sock, F_GETFL) | O_NONBLOCK);

    //register this peer_sock with epoll
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = peer_sock;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, peer_sock, &ev);
    // close(peer_sock);

    return;

}


//makes request messages
unsigned char* reqMessage(int block_length, int block_offset, int piece_index){
    unsigned char* req = (unsigned char*) malloc(17);
    memset(req, 0, 17);
    req[3] = 0xd;
    req[4] = 6;
    memcpy(req + 5, itochar(piece_index), 4);
    memcpy(req + 9, itochar(block_offset*0x4000), 4);
    memcpy(req + 13, itochar(block_length), 4);

    return req;
}


//sets socket to non-blocking
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


//modifies epoll events for a given fd
int modify_epoll_events(int epoll_fd, int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}


//handles incoming data
int handle_receive(int epoll_fd, struct peerStats *peer, struct pieceStats* thisPiece) {

    int msg_id = 9;
    int block_length = 0x4000;
    // unsigned char* stepper = NULL;

    int count = 0; //total bytes received
    unsigned char msg_length[4]; //keeps length of next message in transmission
    int msg_len_int;

    unsigned char interest[5] = {0,0,0,1,2};
    unsigned char incoming_index[4];
    int block_offset;

    while (1) {

        count = recv(peer->sockfd, peer->recv_buffer + peer->recv_buffer_count,BUFFER_SIZE - peer->recv_buffer_count, 0);

        if (count == 0){
            // Peer closed connection
            printf("Peer disconnected fd %d\n", peer->sockfd);
            return -1;
        } else if(count < 0) {
            
            if (errno == EAGAIN || errno == EWOULDBLOCK) {

                // No more data to read now
                break;
            } else {
                perror("recv");
                return -1;
            }
        }

        peer->downloaded += count;
        peer->recv_buffer_count += count;
        peer->last_recv = time(NULL);

        while(1){
            
            if(peer->recv_buffer_count < 4){
                //not enough bytes read for length prefix
                break;
            }
                
            memcpy(msg_length,peer->recv_buffer,4); 
            msg_len_int = chartoi(msg_length);

            //check if full message is in buffer
            if(peer->recv_buffer_count < 4 + msg_len_int){
                //full message not transmitted, wait for more
                break;
            }

            msg_id = peer->recv_buffer[4];
            // printf("msg_id = %d\n",msg_id);

            switch(msg_id){
                case 0://choke

                    //update peer -> am_choked
                    peer->am_choked = true;

                    //queue interest
                    //{0,0,0,1,2}
                    break;

                case 1://unchoke

                    //queue request

                    for(int j = 0; j < thisPiece->block_num; j++){
                        
                        if(j == thisPiece->block_num - 1){
                            block_length = thisPiece->block_remainder;
                        }
                        if(!thisPiece->blocks_have[j]){
                            if(BUFFER_SIZE - peer->send_buffer_count > 17){
                                
                                unsigned char*reqmsg = reqMessage(block_length, j,thisPiece->piece_index);
                                memcpy(peer->send_buffer + peer->send_buffer_count,reqmsg,17);
                                peer->send_buffer_count += 17;
                                modify_epoll_events(epoll_fd, peer->sockfd, EPOLLOUT);
                        
                            }
                            
                        }
                        block_length = 0x4000;

                    }
                    
                    break;

                case 2://interested

                    //update is_interested
                    peer->is_interested = true;

                    //queue unchoke under condition

                    break;

                case 3://not interested

                    //update is_interested
                    peer->is_interested = false;

                    break;
                
                case 4://have

                    //update peer->have
                    peer->have[peer->recv_buffer[5]] = true;
                    break;

                case 5://bitfield

                    //queue interest
                    if(BUFFER_SIZE - peer->send_buffer_count > 5){
                        memcpy(peer->send_buffer,interest,5);
                        peer->send_buffer_count += 5;
                        modify_epoll_events(epoll_fd, peer->sockfd, EPOLLOUT);
                    
                    }
                    break;

                case 6://request

                    //queue piece
                    break;

                case 7://piece
                                        
                    //check piece index is the piece we are seeking
                    memcpy(incoming_index, peer->recv_buffer + 5,4);
                    if(chartoi(incoming_index)!=thisPiece->piece_index){
                        break;
                    }
                    //if it is, read offset
                    memcpy(incoming_index, peer->recv_buffer + 9,4);
                    block_offset = (int)(chartoi(incoming_index)/0x4000);
                    //if we do not have this block
                    if(thisPiece->blocks_have[block_offset] == false){

                        //if this is final block, block_length can be different
                        if(block_offset == thisPiece->block_num - 1){
                            block_length = thisPiece->block_remainder;
                        }

                        //copy block onto thisPiece buffer
                        memcpy(thisPiece->buffer + block_offset*0x4000,peer->recv_buffer + 13,block_length);
                        // recv_buffer + 13 because the buffer contains:
                        // message length (4) + message id (1) 
                        // + payload index (4) + payload begin/offset (4)
                        // + payload block 

                        //reset standard block_length
                        block_length = 0x4000;

                        //record block as received
                        thisPiece->blocks_have[block_offset] = true;

                    } 
                    
                    break;

                    //check hash
                    
                    //queue have to all peers

                case 8://cancel
                    
                    //nothing
                    break;

                default:

                    //nothing
                    break;
            }
            
            // shift message             
            peer->recv_buffer_count = peer->recv_buffer_count - msg_len_int - 4;
            memmove(peer->recv_buffer, peer->recv_buffer + msg_len_int + 4, BUFFER_SIZE - msg_len_int - 4);
            
        }
            
    }
    return 0;
}


//sends data from peer's send buffer
int handle_send(int epoll_fd, struct peerStats *peer) {
    while (1) {

        if(peer->send_buffer_count == 0){break;}

        int sent = send(peer->sockfd, peer->send_buffer,peer->send_buffer_count, 0);

        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Socket send buffer full, wait for EPOLLOUT
            // Ensure EPOLLOUT is enabled
            modify_epoll_events(epoll_fd, peer->sockfd, EPOLLIN | EPOLLOUT);
            return 0;
        } else if(sent <= 0) {
            // Error or connection closed
            perror("send");
            return -1;
        }
        
        peer->uploaded += sent;

        memmove(peer->send_buffer, peer->send_buffer + sent, BUFFER_SIZE - sent);
        peer->send_buffer_count = peer->send_buffer_count - sent;
        sent = 0;
        
    }

    modify_epoll_events(epoll_fd, peer->sockfd, EPOLLIN);
    return 0;
}


//**MAIN**//


int main(int argc, char* argv[]) {
	// Disable output buffering
	setbuf(stdout, NULL);
 	setbuf(stderr, NULL);

    if (argc < 3) {
        fprintf(stderr, "Usage: your_bittorrent.sh <command> <args>\n");
        return 1;
    }

    const unsigned char* command = argv[1];

    if (strcmp(command, "decode") == 0) {
        const unsigned char* encoded_str = argv[2];
        unsigned char* decoded_str = decode_bencode(encoded_str);
        printf("%s\n", decoded_str);
        free(decoded_str);

        return 0;
    }


    if (strcmp(command, "info") == 0){

        if (argc < 2) {
            fprintf(stderr, "Usage: your_bittorrent.sh info <file name>\n");
            return 1;
        }

        const char* file_name = argv[2];
        struct listStruct opened_file = fileOpener(file_name);

        struct torInfo torrent_info = findValues(opened_file.decoded_str);

        printTorInfo(torrent_info);
        
        free(opened_file.decoded_str);
        opened_file.decoded_str = NULL;
        free(torrent_info.announce);
        torrent_info.announce = NULL;
        free(torrent_info.info_hash);
        torrent_info.info_hash = NULL;
        free(torrent_info.piece_hashes);
        torrent_info.piece_hashes = NULL;

        return 0;
    }


    if (strcmp(command, "peers") == 0){
        
        if (argc < 2) {
            fprintf(stderr, "Usage: your_bittorrent.sh peers <file name>\n");
            return 1;
        }
        
        const char* file_name = argv[2];


        //extract file contents
        struct listStruct opened_file = fileOpener(file_name);

        //extract values from info dictionary
        struct torInfo torrent_info = findValues(opened_file.decoded_str);
        //construct URL based on announce URL and make get request
        char* FULL_URL = peers(torrent_info,NULL);
        struct listStruct peerStats = makeGETRequest(FULL_URL);
        
        free(opened_file.decoded_str);
        opened_file.decoded_str = NULL;
        free(torrent_info.announce);
        torrent_info.announce = NULL;
        free(torrent_info.info_hash);
        torrent_info.info_hash = NULL;
        free(torrent_info.piece_hashes);
        torrent_info.piece_hashes = NULL;
        free(FULL_URL);
        FULL_URL = NULL;

        //print (multiple) peer ip:port info
        printPeersAddr(peerStats);

        free(peerStats.decoded_str);

        return 0;
    }


    if (strcmp(command, "handshake") == 0){

        if (argc < 4) {
            fprintf(stderr, "Usage: your_bittorrent.sh handshake <file name> <peer_ip>:<peer_port>\n");
            return 1;
        }

        const char* file_name = argv[2];
        const char* peer_info = argv[3];

        //extract file contents
        struct listStruct opened_file = fileOpener(file_name);
        //extract peer ip:port info
        struct listStruct SERVER_ADDR = portExtract(peer_info);

        //make tcp connection
        int sockfd = tcpConnection(SERVER_ADDR);

        free(SERVER_ADDR.decoded_str);
        SERVER_ADDR.decoded_str = NULL;

        //find peer id
        unsigned char* buffer = peerID(sockfd, opened_file.decoded_str);
            
        close(sockfd);
        free(opened_file.decoded_str);
        opened_file.decoded_str = NULL;

        //print peer id
        printPeerID(buffer);

        free(buffer);
        buffer = NULL;

        return 0;
    }


    if (strcmp(command, "download_piece") == 0){

        if (argc < 6) {
            fprintf(stderr, "Usage: your_bittorrent.sh download_piece -o <save address> <file name> <piece index>\n");
            return 1;
        }
    
        
        const char* save_address = argv[3];
        const char* file_name = argv[4];
        int piece_index = atoi(argv[5]);

        //make GET request, discover all peers
        //extract file contents
        struct listStruct opened_file = fileOpener(file_name);

        //extract values from info dictionary
        struct torInfo torrent_info = findValues(opened_file.decoded_str);
        //construct URL based on announce URL and make get request
        char* FULL_URL = peers(torrent_info,NULL);
        struct listStruct peersAddr = makeGETRequest(FULL_URL);

        int piece_num = (int)(torrent_info.hashes_length/20);//number of pieces in a file
        //pieceStats records the state of what has been downloaded of a single piece
        struct pieceStats thisPiece;
        thisPiece = populate_piece_stats(piece_index, piece_num, &torrent_info);

        //check if thisPiece has all blocks required
        bool check_haveallblocks = true;
        
        //how many peers in total
        int peers_num = (int)(peersAddr.bLength + 1)/6;
        peers_num = peers_num>MAX_PEERS?MAX_PEERS:peers_num;

        //create epoll event (int flags = 0)
        int epoll_fd = epoll_create1(0);

        // for handling events that come in/go out
        //int MAX_EVENTS = 64;
        struct epoll_event events[MAX_EVENTS];

        //create peerStats for each peer
        struct peerStats peer[peers_num];

        int peer_sock;
        int peer_port;
        // unsigned char* peerhash;
            
        //connect to each peer and register each peer socket fd to epoll
        for(int i = 0; i< peers_num; i ++){
            
            struct listStruct SERVER_ADDR;
            
            SERVER_ADDR = ipExtract(peersAddr.decoded_str + 6* i);
            peer_sock = tcpConnection(SERVER_ADDR);
            peer_port = SERVER_ADDR.bLength;

            register_peers(epoll_fd, peer_sock, peer_port, piece_num, &(peer[i]), peersAddr.decoded_str + 6*i, opened_file.decoded_str);
            
            free(SERVER_ADDR.decoded_str);

            // SERVER_ADDR.decoded_str = NULL;
            
        }
            free(opened_file.decoded_str);
            opened_file.decoded_str = NULL;
    


        struct peerStats* peer_ptr;

        //event loop (handles things when they come in via any socket)
        while (1) {
            int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
            if (n == -1) {
                if (errno == EINTR) continue;
                perror("epoll_wait");
                break;
            }

            for (int i = 0; i < n; i++) {
                int fd = events[i].data.fd;

                peer_ptr = NULL;
                for (int j = 0; j < peers_num; j++) {
                    if (peer[j].sockfd == fd) {
                        peer_ptr = &peer[j];
                        break;
                    }
                }

                if (!peer_ptr){ continue;}

                if (events[i].events & EPOLLIN) {

                    if (handle_receive(epoll_fd,peer_ptr,&thisPiece) == -1) {
                        // Close and cleanup on error
                        close(fd); 
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                    }
                    check_haveallblocks = true;
                    for(int j = 0; j < thisPiece.block_num; j++){
                        if(!thisPiece.blocks_have[j]){
                            check_haveallblocks = false;
                            break;
                        }
                    }
                    if(check_haveallblocks){
                        //calculate checksum of thisPiece.buffer
                        unsigned char* hash = (unsigned char*)malloc(SHA_DIGEST_LENGTH);
                        memset(hash, 0x0, SHA_DIGEST_LENGTH); 
                        SHA1(thisPiece.buffer, thisPiece.piece_length, hash);
                        printf("Download hash: ");
                        for(int z = 0; z < 20; z++){
                            printf("%02x",hash[z]);
                        }
                        printf("\n");

                        memcpy(hash,thisPiece.piece_hash,20);
                        printf("Piece hash: ");
                        for(int z = 0; z < 20; z++){
                            printf("%02x",hash[z]);
                        }
                        printf("\n");

                        //compare with thisPiece.piece_hash
                        for(int k = 0; k < 20; k ++){
                            if(hash[k]!= thisPiece.piece_hash[k]){
                                check_haveallblocks = false;
                                printf("Piece donwload failed: hash incorrect.");
                                close(fd);
                                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, NULL);  
                                break;   
                            }
                        }

                        if(check_haveallblocks){
                            //save to buffer
                            FILE* fileptr;
                            fileptr = fopen(save_address,"w");
                            if(fileptr == NULL){
                                perror("Error opening file.");

                            free(torrent_info.announce);
                            torrent_info.announce = NULL;
                            free(torrent_info.info_hash);
                            torrent_info.info_hash = NULL;
                            free(torrent_info.piece_hashes);
                            torrent_info.piece_hashes = NULL;
                            free(thisPiece.piece_hash);
                            thisPiece.piece_hash = NULL;
                            free(thisPiece.buffer);
                            thisPiece.buffer = NULL;
                            free(thisPiece.blocks_have);
                            thisPiece.blocks_have = NULL;

                                return 1;
                            }

                            fwrite(thisPiece.buffer, 1, thisPiece.piece_length, fileptr);
                            fclose(fileptr);
                            close(fd);

                            free(torrent_info.announce);
                            torrent_info.announce = NULL;
                            free(torrent_info.info_hash);
                            torrent_info.info_hash = NULL;
                            free(torrent_info.piece_hashes);
                            torrent_info.piece_hashes = NULL;
                            free(thisPiece.piece_hash);
                            thisPiece.piece_hash = NULL;
                            free(thisPiece.buffer);
                            thisPiece.buffer = NULL;
                            free(thisPiece.blocks_have);
                            thisPiece.blocks_have = NULL;

                            return 0;
                        }

                        
                    }else{continue;}

                }

                if (events[i].events & EPOLLOUT) {
                    if (handle_send(epoll_fd,peer_ptr) == -1) {
                        close(fd);
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, NULL);                    
                    }
                }

                if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                    // Handle hangup or error
                    close(fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                }
            }
        }

        free(torrent_info.announce);
        torrent_info.announce = NULL;
        free(torrent_info.info_hash);
        torrent_info.info_hash = NULL;
        free(torrent_info.piece_hashes);
        torrent_info.piece_hashes = NULL;
        free(thisPiece.piece_hash);
        thisPiece.piece_hash = NULL;
        free(thisPiece.buffer);
        thisPiece.buffer = NULL;
        free(thisPiece.blocks_have);
        thisPiece.blocks_have = NULL;

        return 0;
    }

    if (strcmp(command, "download") == 0){

    if (argc < 4) {
        fprintf(stderr, "Usage: your_bittorrent.sh download -o <save address> <file name>\n");
        return 1;
    }

    // poor man's implementation:
    // download pieces in order rather than asynchronously
    const char* save_address = argv[3];
    const char* file_name = argv[4];

    //open torrent file to discover number of pieces and piece lengths;
    struct listStruct opened_file = fileOpener(file_name);
    struct torInfo torrent_info = findValues(opened_file.decoded_str);
    int piece_num = (int)(torrent_info.hashes_length/20);//number of pieces in a file
    size_t piece_length = torrent_info.piece_length;
    size_t file_length = torrent_info.file_length;


    //create names for files to store each piece
    char piece_address[strlen(save_address)+ 5];
    memset(piece_address,0,strlen(save_address) + 5);
    memcpy(piece_address,save_address,strlen(save_address));

    char* arg_v[6];
    
    arg_v[0] = "program-name";
    arg_v[1] = "download_piece";
    arg_v[2] = "-o";
    arg_v[3] = malloc(strlen(save_address) + 5);
    memset(arg_v[3],0,strlen(save_address) + 5);
    arg_v[4] = (char*)file_name;
    arg_v[5] = malloc(4);
    memset(arg_v[5],0,4);
    //populate arg_v for calling main() with download-piece command
    
    char piece_index[5];
    memset(piece_index,0,5);

    // download pieces in order
    for(int i = 0; i < piece_num; i++){

        memset(arg_v[3],0,strlen(save_address) + 5);
        memset(arg_v[5],0,4);
   
        sprintf(piece_index,"%d",i);
        memcpy(piece_address + strlen(save_address),piece_index,strlen(piece_index) + 1);
        printf("%s\n",piece_address);

        memcpy(arg_v[3],piece_address,strlen(save_address) + 5);
        memcpy(arg_v[5],piece_index,strlen(piece_index));

        main(6, arg_v);
        
    }


    // copy piece files to <save address>, delete piece files
    FILE* fileptr = NULL;
    FILE* savefptr = fopen(save_address,"w");
    char buffer[32768];    
    int count = 0;
    int total_length = 0;

    for(int i = 0; i < piece_num; i++){
        // open piece files
        sprintf(piece_index,"%d",i);
        memcpy(piece_address + strlen(save_address),piece_index,strlen(piece_index) + 1);
        fileptr = fopen(piece_address,"r");

        // write to <save address>
        if(i == piece_num - 1){
            piece_length = file_length - i*piece_length;
        }
        count = fread(buffer,1,piece_length,fileptr);
        
        if(count!=piece_length){
            printf("Did not read properly.\n");
            fclose(fileptr);
            fileptr = NULL;
            break;
        }

        fwrite(buffer,1,count,savefptr);

        total_length += count;
        fclose(fileptr);
        fileptr = NULL;
        count = 0;

        //delete file holding piece read
        if (remove(piece_address) != 0) {
            printf("Error: Unable to delete the file %s.\n",piece_address);
        }

    }

    fclose(savefptr);
  

    return 0;
    
    }

    else {
        fprintf(stderr, "Unknown command: %s\n", command);
        return 1;
    }

    return 0;
}
