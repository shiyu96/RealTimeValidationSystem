/*************************************************
 * Corruption Unit OPENCV Program
 * Contributors: Kamron Ebrahimi, Dafei Du, Pu Huang
 ************************************************/

#include "filters/Distortion.hpp"
#include "filters/Freeze.hpp"
#include "filters/Translate.hpp"
#include "filters/White.hpp"
#include "proto/packet.pb.h"
#include "socket/PracticalSocket.h"
#include "socket/config.h"
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <pthread.h>
#include <queue>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include <zmq.hpp>

/*
 * "192.168.1.122"
 * build prtocol buffer libraries from template .proto file
 * protoc -I=./ --cpp_out=./proto ./packet.proto
 * ./corrupt 192.168.1.124 6666
 */

using packet::FramePacket;
void *handleIPC(void *threadid);
void dispatch(std::vector<std::string> cmd);

/* Enum for each type of distortion */
enum distortion { FREEZE, WHITE, SHIFT };
enum distortion type;

/*
 * Mutex for preventing resource contention between the primary thread and the
 * thread handling the IPC
 */
std::mutex mtx;

/* Vector of distortion filters */
std::vector<Distortion *> dis(3);

int main(int argc, char *argv[]) {

  /* Test for correct number of arguments */
  if ((argc < 3) || (argc > 3)) {
    cerr << "Usage: " << argv[0] << " <Server> <Server Port>\n";
    exit(1);
  }

  /* Connect to the comparison unit */
  std::string servAddress = argv[1];
  TCPSocket sock(servAddress, atoi(argv[2]));

  /* Initialize the distortion filters, push them to the vector */
  type = SHIFT;
  Freeze freeze(100);
  White white(50, 255, FRAME_HEIGHT, FRAME_WIDTH);
  Translate translate(50, FRAME_HEIGHT, FRAME_WIDTH, 15, 15);
  dis[0] = &freeze;
  dis[1] = &white;
  dis[2] = &translate;

  /* Spawn the IPC socket thread */
  pthread_t ipcHandler;
  pthread_create(&ipcHandler, NULL, handleIPC, (void *)1);

  /* Declare all frame capture objects */
  cv::Mat frame, dup, cp1, cp2;
  cv::Mat *dframe;

  /* Open the camera */
  cv::VideoCapture cap(0);
  if (!cap.isOpened()) {
    cerr << "OpenCV Failed to open camera";
    exit(1);
  }

  while (1) {

    /*
     * Read an image frame, produce a replica, pass the replicated image into
     * the distortion filter. Lock the mutex when applying the filter to
     * ensure the IPC thread does not configure the filter synchronously
     */
    cap >> frame;
    dup = frame.clone();
    dframe = &frame;
    mtx.lock();
    dis[type]->run(dframe);
    mtx.unlock();

    /*
     * Create containers to place the compressed images inside and vector
     * of compression parameters. Compress the images to .jpg format
     */
    std::vector<uchar> compImgA, compImgB;
    std::vector<int> compression_params;
    compression_params.push_back(cv::IMWRITE_JPEG_QUALITY);
    compression_params.push_back(ENCODE_QUALITY);
    cv::imencode(".jpg", dup, compImgA, compression_params);
    cv::imencode(".jpg", *dframe, compImgB, compression_params);

    /*
     * Populate the FramePacket generated from the .proto template.
     * The packet will contain the dimensions of the compressed images,
     * the amount of data and the 3D RGB matrix
     */
    FramePacket packet;
    std::string serial_dat;
    packet.set_rows(dup.rows);
    packet.set_cols(dup.cols);
    packet.set_rowsb(dframe->rows);
    packet.set_colsb(dframe->cols);
    packet.set_elt_type(dframe->type());
    packet.set_elt_sizea(compImgA.size());
    packet.set_elt_sizeb(compImgB.size());
    packet.set_mat_dataa(compImgA.data(), compImgA.size());
    packet.set_mat_datab(compImgB.data(), compImgB.size());

    /*
     * Serialize the packet into a String object
     */
    if (!packet.SerializeToString(&serial_dat))
      std::cout << "failed to serialize data" << std::endl;

    /*
     * Send the serialized frame over the wire in fragments of
     * PACK_SIZE. First send the total number of incoming bytes for the frame
     * synced packet to the server, then we send the data.
     */
    int total_pack = 1 + (serial_dat.size() - 1) / PACK_SIZE;
    int ibuf[1];
    ibuf[0] = serial_dat.size();
    std::cout << "sending " << serial_dat.size() << std::endl;
    sock.send(ibuf, sizeof(int));
    for (int i = 0; i < total_pack; i++)
      sock.send(&serial_dat[i * PACK_SIZE], PACK_SIZE);
  }

  return 0;
}

/*
 * Second thread of execution for handling all incoming
 * input from the web server. This thread opens and maintains
 * a ZeroMQ socket on port 5555
 */
void *handleIPC(void *threadid) {
  /* Prepare our context and socket */
  zmq::context_t context(1);
  zmq::socket_t socket(context, ZMQ_REP);
  socket.bind("tcp://*:5555");

  while (true) {
    zmq::message_t request;
    /* Wait for a request from the web server */
    socket.recv(&request);

    /* Process the data into a parsable format */
    std::string cmd_str =
        std::string(static_cast<char *>(request.data()), request.size());
    std::stringstream ss(cmd_str);
    std::vector<std::string> cmd;
    while (ss.good()) {
      std::string substr;
      getline(ss, substr, ',');
      cmd.push_back(substr);
    }

    /*
     * Lock the mutex and update the filter to ensure that there is no resource
     * contention with the primary thread
     */
    mtx.lock();
    dispatch(cmd);
    mtx.unlock();

    /* Send reply back to client */
    zmq::message_t reply(3);
    memcpy(reply.data(), "ACK", 3);
    socket.send(reply);
  }
}

/*
 * Format (use .join(',') method on a dynamically constructed list object) to
 * produce these commands Freeze:   ['1', '1/0', 'dur(ms)'] White:    ['2',
 * '1/0', 'dur(ms)', 'shade(0-255)'] Translate ['3', '1/0', 'dur(ms)',
 * 'x_offset', 'y_offset']
 */
void dispatch(std::vector<std::string> cmd) {
  switch (stoi(cmd[0], nullptr)) {
  case 1:
    type = FREEZE;
    dis[type]->update(cmd);
    break;
  case 2:
    type = WHITE;
    dis[type]->update(cmd);
    break;
  case 3:
    type = SHIFT;
    dis[type]->update(cmd);
    break;
  }
}
