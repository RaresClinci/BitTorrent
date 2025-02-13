#include <mpi.h>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <set>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <ctime>

using namespace std;

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

#define ACK_TAG 1
#define INIT_DONE_TAG 2
// requesting file from tracker
#define REQUEST_FILE 3
// requesting peers from tracker
#define REQUEST_UPDATE 4
// client downloaded the file
#define FILE_DONE 5
// requesting file from a peer
#define SEGMENT_REQUEST 6
#define SEGMENT_RESPONSE 7
// client downloaded all files
#define CLIENT_DONE 8
// everyone downloaded the files
#define EVERYONE_DONE 9
// tag for sending peer info
#define PEER_LEN 10
#define PEER_TAG 11

// all segments are indexed from 0 to 99 (there are a maximum of 100 segments)
#define INDEX_LEN 2

// all 3 are used by the upload thread
// OK - peer has the segment, NO - peer doesn't have the segment
const string OK = "ok";
const string NO = "no";
// message for upload thread to stop
const string STOP = "stop";
// above message's lengths
#define SEG_RESP_LEN 2
#define STOP_LEN 4

#define UPPER_LIMIT 10

// file info(tracker)
struct File_Info{
    vector<string> segments;
    set<int> peers;
};

// file info(client)
struct File_Info_Simple{
    vector<string> segments;
    vector<int> peers;
};


// dummy for blank messages
int dummy;

// TRACKER VARIABLES
unordered_map<string, File_Info> centralizer;

// CLIENT VARIABLES
int num_files;
unordered_map<string, vector<string>> segment_list;
pthread_mutex_t segment_mutex;
vector<string> files_needed;

// send string with MPI
void mpi_send_string(const string& message, int length, int destination, int tag, MPI_Comm communicator) {
    // copy string to buffer
    char buffer[length + 1] = { '\0' };
    std::copy(message.begin(), message.end(), buffer);

    // send message
    MPI_Send(buffer, length + 1, MPI_CHAR, destination, tag, communicator);
}

// recv string with MPI
void mpi_recv_string(string& message, int length, int source, int tag, MPI_Comm communicator, MPI_Status& status) {
    // receive length
    char* buffer = new char[length + 1];
    MPI_Recv(buffer, length + 1, MPI_CHAR, source, tag, communicator, &status);
    buffer[length] = '\0';

    // set to string
    message = std::string(buffer);

    delete[] buffer;
}

// numbering the segment
string assign_order(string segment, int num) {
    if(num < 10)
        return "0" + to_string(num) + segment;
    else 
        return to_string(num) + segment;
}

// extracting the segmnet number
int extract_order(string msg) {
    string num_str = msg.substr(0, 2);
    return stoi(num_str);
}

// extracting the segment
string extract_segment(string msg) {
    return msg.substr(2);
}

// request a file function
File_Info_Simple request_file(string filename, int rank) {
    mpi_send_string(filename, FILENAME_MAX, TRACKER_RANK, REQUEST_FILE, MPI_COMM_WORLD);

    // receving the data
    MPI_Status status;
    File_Info_Simple file_info;

    // receiving number of segments
    int num_segments;
    MPI_Recv(&num_segments, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, &status);

    // receiving segments
    file_info.segments.resize(num_segments);
    for (int i = 0; i < num_segments; i++) {
        string msg;
        mpi_recv_string(msg, HASH_SIZE + INDEX_LEN, TRACKER_RANK, 0, MPI_COMM_WORLD, status);

        // extracting the info from the message
        int idx = extract_order(msg);
        string seg = extract_segment(msg);

        file_info.segments[idx] = seg;
    }

    // receiving number of peers
    int num_peers;
    MPI_Recv(&num_peers, 1, MPI_INT, TRACKER_RANK, PEER_LEN, MPI_COMM_WORLD, &status);

    // receiving peers
    file_info.peers.resize(num_peers);
    for (int i = 0; i < num_peers; i++) {
        MPI_Recv(&file_info.peers[i], 1, MPI_INT, TRACKER_RANK, PEER_TAG, MPI_COMM_WORLD, &status);
    }

    return file_info;
}

// request peers function
vector<int> request_peers(string filename, int rank) {
    mpi_send_string(filename, FILENAME_MAX, TRACKER_RANK, REQUEST_UPDATE, MPI_COMM_WORLD);

    // receving the data
    MPI_Status status;
    vector<int> peers;

    // receiving number of peers
    int num_peers;
    MPI_Recv(&num_peers, 1, MPI_INT, TRACKER_RANK, PEER_LEN, MPI_COMM_WORLD, &status);

    // receiving peers
    peers.resize(num_peers);
    for (int i = 0; i < num_peers; i++) {
        MPI_Recv(&peers[i], 1, MPI_INT, TRACKER_RANK, PEER_TAG, MPI_COMM_WORLD, &status);
    }

    return peers;
}

// write the hashes in the file
void write_to_file(int rank, string filename) {
    // creating file name
    string out_file = "client" + to_string(rank) + "_" + filename;
    ofstream fout(out_file);

    // writing segments
    vector<string> segments = segment_list[filename];

    for(string& seg : segments) {
        fout << seg << endl;
    }

    fout.close();
}

void *download_thread_func(void *arg)
{
    int rank = *(int*) arg;
    srand(time(NULL) + rank);

    MPI_Status status;
    for (string& file : files_needed) {
        // requesting file
        File_Info_Simple file_info = request_file(file, rank);

        // requesting the segments from peers
        int n = file_info.peers.size();
        for(string& segment : file_info.segments) {
            // checking whether we have the segment
            if (segment_list.find(file) != segment_list.end()) {
                if (find(segment_list[file].begin(), segment_list[file].end(), segment) != segment_list[file].end()) 
                    continue;
            } else {
                pthread_mutex_lock(&segment_mutex);
                segment_list[file] = vector<string>();
                pthread_mutex_unlock(&segment_mutex);
            }

            // number of segments received
            int num_segments = 0;

            // searching for segment
            int i = rand() % n;
            while (true) {
                // sending request for segment
                mpi_send_string(file, FILENAME_MAX, file_info.peers[i], SEGMENT_REQUEST, MPI_COMM_WORLD);
                mpi_send_string(segment, HASH_SIZE, file_info.peers[i], SEGMENT_REQUEST, MPI_COMM_WORLD);

                // waiting for replies
                string msg;
                mpi_recv_string(msg, HASH_SIZE, MPI_ANY_SOURCE, SEGMENT_RESPONSE, MPI_COMM_WORLD, status);

                // adding response 
                if (msg == OK) {
                    pthread_mutex_lock(&segment_mutex);
                    segment_list[file].push_back(segment);
                    pthread_mutex_unlock(&segment_mutex);

                    // updating peer list
                    num_segments++;
                    if (num_segments % 10 == 0) {
                        file_info.peers = request_peers(file, rank);
                    }

                    break;
                }
 
                // asking the next peer
                i = (i + 1) % n;
            }

        }
        // telling the tracker the file is done
        mpi_send_string(file, FILENAME_MAX, TRACKER_RANK, FILE_DONE, MPI_COMM_WORLD);

        write_to_file(rank, file);
    }

    // announcing the tracker that the client is done
    mpi_send_string("", FILENAME_MAX, TRACKER_RANK, CLIENT_DONE, MPI_COMM_WORLD);

    return NULL;
}

void *upload_thread_func(void *arg)
{
    int rank = *(int*) arg;

    while (true) {
        // receiving a request
        string file, segment;
        MPI_Status status, status2;
        mpi_recv_string(file, FILENAME_MAX, MPI_ANY_SOURCE, SEGMENT_REQUEST, MPI_COMM_WORLD, status);
        mpi_recv_string(segment, HASH_SIZE, status.MPI_SOURCE, SEGMENT_REQUEST, MPI_COMM_WORLD, status2);

        // checking whether everyone is done
        if (segment == STOP) {
            break;
        }

        // replying to request
        pthread_mutex_lock(&segment_mutex);
        if (find(segment_list[file].begin(), segment_list[file].end(), segment) != segment_list[file].end()) {
            // the client has the segment
            mpi_send_string(OK, SEG_RESP_LEN, status.MPI_SOURCE, SEGMENT_RESPONSE, MPI_COMM_WORLD);
        } else {
            // the cleint doesn't have the segment
            mpi_send_string(NO, SEG_RESP_LEN, status.MPI_SOURCE, SEGMENT_RESPONSE, MPI_COMM_WORLD);
        }
        pthread_mutex_unlock(&segment_mutex);
    }

    return NULL;
}

// centralize all files in the tracker
void centralize_files(int numtasks) {
    unordered_map<string, int> responsible;
    for (int client = 0; client < numtasks; client++) {
        if (client == TRACKER_RANK)
            continue;
        
        // geting number of files
        int num_files;
        MPI_Recv(&num_files, 1, MPI_INT, client, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        // going through the files
        for(int f_no = 0; f_no < num_files; f_no++) {
            // status
            MPI_Status status;

            // getting filename
            string filename;
            mpi_recv_string(filename, FILENAME_MAX, client, 0, MPI_COMM_WORLD, status);

            if (responsible.find(filename) == responsible.end()) {
                responsible[filename] = client;
            }

            // getting number of segments
            int num_segments;
            MPI_Recv(&num_segments, 1, MPI_INT, client, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            if (centralizer.find(filename) == centralizer.end()) {
                centralizer[filename] = File_Info();
                centralizer[filename].segments.resize(num_segments);
                centralizer[filename].peers.insert(client);
            }

            // iterating through the segments
            for (int s_no = 0; s_no < num_segments; s_no++) {
                string msg;
                mpi_recv_string(msg, HASH_SIZE + INDEX_LEN, client, 0, MPI_COMM_WORLD, status);

                // only the responsible client adds the segments
                if(responsible[filename] == client) {    
                    // extracting info from message
                    int idx = extract_order(msg);
                    string segment_hash = extract_segment(msg);

                    // adding the segment and the client
                    centralizer[filename].segments[idx] = segment_hash;
                }
                
            }
        }

    }
}

// tracker tells everyone the initialization step is done
void init_finish_bcast(int numtasks) {
    for (int i = 0; i < numtasks; i++) {
        MPI_Send(&dummy, 1, MPI_INT, i, INIT_DONE_TAG, MPI_COMM_WORLD);
    }
}

// tracker sends file info
void send_file_info(string filename, int rank) {
    File_Info& file_info = centralizer[filename];

    // sending number of segmnets
    int num_segments = file_info.segments.size();
    MPI_Send(&num_segments, 1, MPI_INT, rank, 0, MPI_COMM_WORLD);

     // sending segments
    for (int i = 0; i < num_segments; i++) {
        string msg = assign_order(file_info.segments[i], i);
        mpi_send_string(msg, HASH_SIZE + INDEX_LEN, rank, 0, MPI_COMM_WORLD);
    }

    // sending the number of peers
    int num_peers = file_info.peers.size();
    MPI_Send(&num_peers, 1, MPI_INT, rank, PEER_LEN, MPI_COMM_WORLD);

    // sending peers
    for (const int& peer : file_info.peers) {
        MPI_Send(&peer, 1, MPI_INT, rank, PEER_TAG, MPI_COMM_WORLD);
    }
}

// tracker sends file's peers
void send_peer_info(string filename, int rank) {
    File_Info& file_info = centralizer[filename];

    // sending the number of peers
    int num_peers = file_info.peers.size();
    MPI_Send(&num_peers, 1, MPI_INT, rank, PEER_LEN, MPI_COMM_WORLD);

    // sending peers
    for (const int& peer : file_info.peers) {
        MPI_Send(&peer, 1, MPI_INT, rank, PEER_TAG, MPI_COMM_WORLD);
    }
}

// tracker signals all uploader threads to stop
void stop_signal(int numtasks) {
    for (int i = 0; i < numtasks; i++) {
        if(i == TRACKER_RANK)
            continue;

        // signaling the uploader
        mpi_send_string(STOP, FILENAME_MAX, i, SEGMENT_REQUEST, MPI_COMM_WORLD);
        mpi_send_string(STOP, HASH_SIZE, i, SEGMENT_REQUEST, MPI_COMM_WORLD);
    }
}

void tracker(int numtasks, int rank) {
    centralize_files(numtasks);
    init_finish_bcast(numtasks);

    MPI_Status status;
    string message;
    int num_finnished = 0;

    while (true) {
        // waiting for message
        mpi_recv_string(message, FILENAME_MAX, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, status);

        // processing the message
        if (status.MPI_TAG == REQUEST_FILE) {
            send_file_info(message, status.MPI_SOURCE);

            // adding client to peer list
            centralizer[message].peers.insert(status.MPI_SOURCE);
        } else if (status.MPI_TAG == REQUEST_UPDATE) {
            send_peer_info(message, status.MPI_SOURCE);
        } else if (status.MPI_TAG == FILE_DONE) {
            // client became a seed
        } else if (status.MPI_TAG == CLIENT_DONE) {
            num_finnished++;

            // signaling everyone to stop
            if(num_finnished == numtasks - 1) {
                stop_signal(numtasks);
                break;
            }
        }
    }

}

// parsing client input
void parse_input(int rank) {
    ifstream fin("in" + to_string(rank) + ".txt");

    // number of files
    fin >> num_files;

    // reading contents of the files
    for (int i = 0; i < num_files; i++) {
        string file_name;
        int num_segments;

        // reading filename
        fin >> file_name >> num_segments;

        // reading each of the file's segments
        vector<string> segments(num_segments);
        for (int j = 0; j < num_segments; j++) {
            fin >> segments[j];
        }

        segment_list[file_name] = segments;
    }

    // reading number of wanted files
    int wanted_files = 0;
    fin >> wanted_files;

    // reading the names of the wanted files
    for (int i = 0; i < wanted_files; i++) {
        string wanted;
        fin >> wanted;

        files_needed.push_back(wanted);
    }

    fin.close();
}

void send_all_files() {
    // send files
    MPI_Send(&num_files, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);
    for (auto& file : segment_list) {
        string file_name = file.first;
        vector<string> segments = file.second;

        // sending file name
        mpi_send_string(file_name, FILENAME_MAX, TRACKER_RANK, 0, MPI_COMM_WORLD);

        // sending number of segments
        int num_seg = segments.size();
        MPI_Send(&num_seg, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

        // sending segments
        for (int i = 0; i < num_seg; i++) {
            string msg = assign_order(segments[i], i);
            mpi_send_string(msg, HASH_SIZE + INDEX_LEN, TRACKER_RANK, 0, MPI_COMM_WORLD);
        }
    }
}

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    // reading the input from files
    parse_input(rank);

    // sending all files to the tracker
    send_all_files();

    // waiting for the tracker to finish
    MPI_Recv(&dummy, 1, MPI_INT, TRACKER_RANK, INIT_DONE_TAG, MPI_COMM_WORLD, NULL);

    // initializing mutex
    pthread_mutex_init(&segment_mutex, NULL);

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}
 
int main (int argc, char *argv[]) {
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
