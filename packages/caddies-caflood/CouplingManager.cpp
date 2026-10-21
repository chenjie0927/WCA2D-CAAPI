/*
 * CouplingManager.cpp
 *
 *  Created on: Jun 15, 2021
 *      Author: sunelma
 */

#include "ca2D.hpp"
#include "ArgsData.hpp"
#include "CouplingManager.hpp"
#include "Utilities.hpp"
#include <iostream>
#include <fstream>
#include CA_2D_INCLUDE(addInflow)
#include CA_2D_INCLUDE(addCoupledFlow)

#ifdef _WIN32
#include <winsock.h>
#pragma comment(lib, "Ws2_32.lib")
#define ssize_t long
#define SHUT_WR 1
#define SHUT_RDWR 2
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

//! Define a small inflow to ignore.
#define SMALL_INFLOW  1.E-7


int initICouplingsFromCSV(const std::string& filename, std::vector<ICoupling>& couplings)
{
    std::ifstream ifile(filename.c_str());
    
    if(!ifile)
    {
        std::cerr<<"Error opening couplings CSV file: "<<filename<<std::endl;
        return 1;
    }
    
    // Parse the file line by line until the end of file 
    // and retrieve the tokens of each line.
    while(!ifile.eof())
    {   
      bool found_tok = false;
        std::vector<std::string> tokens( CA::getLineTokens(ifile, ',') );

        // If the tokens vector is empty we reached the eof or an
        // empty line... continue.
        if(tokens.empty())
            continue;

        if (tokens.size() < 3)
        {
            std::cerr<<"Not enough tokens for a coupling"<<std::endl;
            return 1;
        }

        ICoupling coupling;
        {
            std::string str;
            READ_TOKEN(found_tok,str,tokens[0],tokens[0]);

            coupling.name = CA::trimToken(str," \t\r");
        }

        if (tokens.size() == 3)
        {
            CA::Real value;
            READ_TOKEN(found_tok, value, tokens[1], tokens[0]);
            coupling.x = value;
            READ_TOKEN(found_tok, value, tokens[2], tokens[0]);
            coupling.y = value;
            coupling.w = 1;
            coupling.h = 1;
        }
        else if (tokens.size() >= 5)
        {
            CA::Real value;
            CA::Unsigned size;
            READ_TOKEN(found_tok, value, tokens[1], tokens[0]);
            coupling.x = value;
            READ_TOKEN(found_tok, value, tokens[2], tokens[0]);
            coupling.y = value;

            READ_TOKEN(found_tok, size, tokens[3], tokens[0]);
            coupling.w = size;
            READ_TOKEN(found_tok, size, tokens[4], tokens[0]);
            coupling.h = size;
        }

        coupling.head = 0;
        coupling.flow = 0;
        couplings.push_back(coupling);
    }
    
    return 0;
}


CouplingManager::CouplingManager(CA::Grid&  GRID, CA::CellBuffReal& ELV, std::vector<ICoupling>& aCoupling, CA::Real aTime_start, CA::Real aTime_end, int aPort):
  grid(GRID),
  coupling(aCoupling),
  time_start(aTime_start),
  time_end(aTime_end),
  port(aPort),
  readValuesUntil(0),
  previousValuesUntil(0),
  networkWaitingUntil(-1),
  sockfd(0),
  points() {

    if (port > 0) {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != NO_ERROR) {
            std::cerr << "Error in WSAStartup" << std::endl;
            throw;
        }
#endif

        sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(sockfd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            std::cerr << "Cannot connect to the coupling host at 127.0.0.1:" << port << std::endl;
#ifdef _WIN32
            closesocket(sockfd);
#endif
            throw;
        }

        // Set the socket to linger for 10 seconds on close
        linger linger { 1, 10 };
        setsockopt(sockfd, SOL_SOCKET, SO_LINGER, (char*)&linger, sizeof(linger));
    }

    bufferSize = createBoxes();
    waterDepthBuffer = new CA::Real[bufferSize];
    readElevations(ELV);
}

CouplingManager::~CouplingManager() {
    delete[] waterDepthBuffer;
}

//! Add the computational domain of the coupled points into the given domain.
void CouplingManager::addDomain(CA::BoxList& compdomain)
{
    for (auto & iter : coupling) {
        compdomain.add(iter.box_area);
    }
}

void CouplingManager::write(const std::string& line) const {
    if (port <= 0) {
        std::cout << line;
        std::cout.flush();
    }
    else {
        const char *data = line.c_str();
        unsigned long length = line.length();
        ssize_t pos = 0;
        while (length > 0) {
            ssize_t bytes = send(sockfd, &data[pos], length, 0);
            if (bytes > 0) {
                pos += bytes;
                length -= bytes;
            }
            else
                break;
        }
    }
}

std::string CouplingManager::read() const {
    if (port <= 0) {
        if (!std::cin.good() || std::cin.eof())
            return "";

        std::string line;
        std::getline(std::cin, line, '\n');
        return line;
    }
    else {
        std::stringstream line("");
        char c = '\n';
        ssize_t len;
        len = recv(sockfd, &c, 1, 0);
        while (len > 0 && c != '\n') {
            line << c;
            len = recv(sockfd, &c, 1, 0);
        }
        return line.str();
    }
}

void CouplingManager::input(CA::Real time) {
    // Nothing to do as there are enough data
    if (inputEnded || stopped || coupling.empty())
        return;

    // There won't be anything input, if the network simulator
    // is waiting for values after the current time.
    if (readValuesUntil >= time - 0.001 || networkWaitingUntil >= time - 0.01) {
        // ...except if there is input waiting to be processed
        fd_set set;
        struct timeval tval = {0, 0};
        FD_ZERO(&set);
        FD_SET(sockfd, &set);
        if (select(1, &set, NULL, NULL, &tval) <= 0)
            return;
    }

    // Inform the other end, that we are actually waiting to get some values in
    std::stringstream line("");

    line << "WAITING," << time << "\n";
    write(line.str());

    std::string readLine;
    do {
        readLine = read();
        std::stringstream readLineStream(readLine);
        std::vector<std::string> tokens(CA::getLineTokens(readLineStream, ','));
        if (!tokens.empty()) {
            if (tokens[0] == "END") {
                inputEnded = true;
                shutdown(sockfd, SHUT_RDWR);
                break;
            } else if (tokens[0] == "STOP") {
                inputEnded = true;
                stopped = true;
                shutdown(sockfd, SHUT_RDWR);
                break;
            } else if (tokens[0] == "WAITING") {
                CA::Real newTime;
                if (!CA::fromString(newTime, tokens[1]))
                    break;

                networkWaitingUntil = newTime;
                if (newTime > readValuesUntil)
                    readValuesUntil = newTime;

                if (newTime >= time - 0.0001)
                    break;
            } else if (tokens[0] == "FLOW") {
                CA::Real newTime;
                previousValuesUntil = readValuesUntil;
                if (!CA::fromString(newTime, tokens[1]))
                    continue;

                readValuesUntil = newTime;

                for (int index = 0; index < tokens.size() - 2 && index < coupling.size(); index++) {
                    CA::Real flow;
                    if (!CA::fromString(flow, tokens[index + 2]))
                        continue;

                    coupling[index].prevFlow = coupling[index].flow;
                    coupling[index].flow = flow;
                }
            }
        }
    } while (readValuesUntil < time - 0.001 && !inputEnded && !readLine.empty());
}

void CouplingManager::output(CA::Real time, CA::CellBuffReal& WD, CA::CellBuffReal& ELV) {
    if (coupling.empty() || stopped || inputEnded)
        return;

    // No need to output values if nobody is going to use them
    if (time < networkWaitingUntil)
        return;

    std::stringstream line("");
    line << "HEAD," << time;

    for (auto& point : coupling) {
        line << "," << point.depth << "," << point.head;
    }
    line << "\n";
    write(line.str());
}

void CouplingManager::add(CA::CellBuffReal& WD, CA::CellBuffState& MASK, const CA::Real area, const CA::Real t, const CA::Real dt)
{
    int numberOfUpdates = 0;
    if (coupling.empty())
        return;

    WD.retrievePoints(points, waterDepthBuffer, bufferSize);

    // Loop through the couplings
    for (auto & point : coupling) {
        // Compute the inflow volume at specific time using
        // interpolation. Check if the index is the last available
        // one, then there is no inflow.
        double volume;
        if (t < readValuesUntil)
            volume = point.prevFlow * dt;
        else
            volume = point.flow * dt;

        unsigned cells = point.box_area.size();

        // Calculate the current average depth
        double depth = calculateAverage(point);

        // Add (or subtract) the given volume into the water depth of the given area.
        // Do not add it if it is close to zero.
        if (std::abs(volume) >= SMALL_INFLOW) {
            // Calculate the average volume to add/remove per cell.
            if (cells > 1)
                volume = volume / (CA::Real)cells;

            // Add the volume to all the cells
            double newDepth = 0;
            coupledVolume += updateFlow(point, volume / area, depth, newDepth) * area;
            point.depth = (CA::Real)newDepth;
            point.head = point.elv + point.depth;
            numberOfUpdates++;
        }
        else {
            // Update the new average depth and head at the point
            point.depth = (CA::Real)depth;
            point.head = (CA::Real)(point.elv + depth);
        }
    }

    if (numberOfUpdates > 0)
        WD.insertPoints(points, waterDepthBuffer, bufferSize);
}

double CouplingManager::calculateAverage(ICoupling& point) {
    const unsigned w = point.w;
    const unsigned h = point.h;

    unsigned cells = 0;
    double average = 0;

    for (unsigned y = 0; y < h; y++) {
        unsigned yPos = y * point.w;
        for (unsigned x = 0; x < w; x++) {
            CA::Real value = waterDepthBuffer[point.offset + yPos + x];
            if (value > 0) {
                average += value;
                cells++;
            }
        }
    }

    if (cells > 0)
        return average / cells;
    else
        return 0;
}

double CouplingManager::updateFlow(ICoupling& point, const double depth, const double avg, double &newAvg) {
    const unsigned w = point.w;
    const unsigned h = point.h;

    newAvg = 0.0;
    unsigned cells = 0;
    double outputVolume = 0;
    for (unsigned y = 0; y < h; y++) {
        unsigned yPos = y * point.w;
        for (unsigned x = 0; x < w; x++) {
            CA::Real wd = waterDepthBuffer[point.offset + yPos + x];

            if (wd < 0)
                continue;
            else if (depth == 0) {
                newAvg += wd;
                cells++;
            }
            else {
                // Compute the amount of water to add or remove.
                double newLevel;
                if (depth >= 0 || fabs(avg) < 1e-5)
                    newLevel = fmax(0.0, wd + depth);
                else if (depth < 0) {
                    double diff = (wd / avg) * depth;
                    newLevel = fmax(0.0, wd + diff);
                }
                else
                    newLevel = wd;

                waterDepthBuffer[point.offset + yPos + x] = (CA::Real) newLevel;
                newAvg += newLevel;
                outputVolume += (newLevel - wd);
                cells++;
            }
        }
    }

    newAvg /= cells;
    return outputVolume;
}

CA::Unsigned CouplingManager::createBoxes()
{
    CA::Unsigned totalSize = 0;
    std::cout<<"---- COUPLING POINTS ----"<<std::endl;
    for (auto & item: coupling)
    {
        CA::Point     tl( CA::Point::create(grid, item.x, item.y) );

        item.box_area = CA::Box(tl.x(), tl.y(), item.w, item.h);
        std::cout<<item.name<< " " << item.x << " " << item.y << " box: " << item.box_area.x() << " " << item.box_area.y() << " "  << item.box_area.w() << " " << item.box_area.h() << std::endl;

        int numPoints = 0;
        for (int y = 0; y < item.h; y++)
            for (int x = 0; x < item.w; x++) {
                points.add(CA::Point(tl.x() + x, tl.y() + y));
                numPoints++;
            }

        item.offset = totalSize;
        totalSize += numPoints;
    }
    return totalSize;
}

void CouplingManager::readElevations(CA::CellBuffReal& ELV) {
    ELV.retrievePoints(points, waterDepthBuffer, bufferSize);
    for (auto & point : coupling) {
        unsigned cells = 0;
        double elevation = 0;
        for (int y = 0; y < point.h; y++) {
            for (int x = 0; x < point.w; x++) {
                int yPos = y * point.w;
                double elv = waterDepthBuffer[point.offset + yPos + x];
                if (elv > -1000) {
                    elevation += elv;
                    cells++;
                }
            }
        }

        if (cells > 0)
            point.elv = (CA::Real)(elevation / cells);
        else
            point.elv = 0;
    }
}

void CouplingManager::end() {
    if (!coupling.empty()) {
        write("END\n");
        shutdown(sockfd, SHUT_RDWR);
    }
}

void CouplingManager::close() {
    // Shut down the socket and ensure all the data is sent for sure
    shutdown(sockfd, SHUT_RDWR);

    // Finally close the socket
#ifdef _WIN32
    closesocket(sockfd);
#else
    ::close(sockfd);
#endif
}

CA::Real CouplingManager::potentialVA(CA::Real t, CA::Real period_time_dt)
{
    CA::Real potential_va = 0.0;

    for (auto & item : coupling)
    {
        CA::Real wd = item.flow / (CA::Real)item.box_area.size() / grid.area();

        // Compute the potential velocity.
        potential_va = std::max(potential_va, std::sqrt(wd * static_cast<CA::Real>(9.81)));
    }

    // ATTENTION! This does not need to be precise but just give a rough estimation
    return potential_va;
}


CA::Real CouplingManager::endTime()
{
    if (coupling.size() == 0)
        return time_start;
    else
        return time_end;
}
