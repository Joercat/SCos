
#pragma once
bool initFS();
const char* readFile(const char* path);
bool writeFile(const char* path, const char* data);
#ifndef RAMFS_HPP
#define RAMFS_HPP

// Function declarations for RAM filesystem
bool initFS();
const char* readFile(const char* path);
bool writeFile(const char* path, const char* data);

#endif
