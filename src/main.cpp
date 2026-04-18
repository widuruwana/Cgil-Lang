#include "../../include/Lexer/Lexer.h"
#include "../../include/Parser/Parser.h"
#include "../../include/Semantics/SemanticAnalyzer.h"
#include "../../include/CodeGen/CodeGen.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio> // For std::remove

void printUsage() {
    std::cout << "Cgil Compiler Forge v1.0\n"
              << "Usage: cgilc <file1.gil> [file2.gil ...] [options]\n\n"
              << "Options:\n"
              << "  -o <file>        Specify the output executable name\n"
              << "  --emit-c         Stop after transpilation; do not invoke GCC, keep .c file\n"
              << "  --target=host    Compile as a standard desktop application (default)\n"
              << "  --target=kernel  Compile as bare-metal OS (applies ISR & hardware GCC flags)\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    std::vector<std::string> inputFiles; // THE FIX: Support multiple files
    std::string outputFile = "";
    bool emitC = false;
    bool targetKernel = false;

    // 1. Parse Command Line Arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--emit-c") {
            emitC = true;
        } else if (arg == "--target=kernel") {
            targetKernel = true;
        } else if (arg == "--target=host") {
            targetKernel = false;
        } else if (arg == "-o") {
            if (i + 1 < argc) {
                outputFile = argv[++i];
            } else {
                std::cerr << "Error: -o requires a filename.\n";
                return 1;
            }
        } else if (arg[0] != '-') {
            inputFiles.push_back(arg);
        } else {
            std::cerr << "Unknown flag: " << arg << "\n\n";
            printUsage();
            return 1;
        }
    }

    if (inputFiles.empty()) {
        std::cerr << "Error: No input .gil file specified.\n";
        return 1;
    }

    // Determine filenames based on the FIRST input file or the -o flag
    std::string firstBaseName = inputFiles[0].substr(0, inputFiles[0].find_last_of('.'));
    
    if (outputFile.empty()) {
        outputFile = firstBaseName;
        if (!emitC && !targetKernel) {
            // Append .exe for Windows host builds if no explicit -o is given
            #ifdef _WIN32
            outputFile += ".exe";
            #endif
        } else if (!emitC && targetKernel) {
            // Bare metal usually outputs .o or .bin by default
            outputFile += ".o";
        }
    }

    // Determine intermediate .c filename based on the output executable name
    std::string cFilename = outputFile;
    size_t dotPos = cFilename.find_last_of('.');
    if (dotPos != std::string::npos) {
        cFilename = cFilename.substr(0, dotPos) + ".c";
    } else {
        cFilename += ".c";
    }

    try {
        ProgramNode program; // The Unified Master AST
        
        std::cout << "[1/4] Reading and Parsing " << inputFiles.size() << " file(s)...\n";
        
        // 2. Read, Lex, and Parse ALL Source Files into the Unified AST
        for (const auto& inputFile : inputFiles) {
            std::ifstream file(inputFile);
            if (!file.is_open()) {
                std::cerr << "Failed to open source file: " << inputFile << "\n";
                return 1;
            }
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string source = buffer.str();

            Lexer lexer(source);
            auto tokens = lexer.tokenize();
            Parser parser(tokens);
            
            // Parse the file and move its declarations into the master program tree
            auto fileDeclarations = parser.parse();
            for (auto& decl : fileDeclarations) {
                program.declarations.push_back(std::move(decl));
            }
        }

        std::cout << "[2/4] AST Merged Successfully.\n";

        std::cout << "[3/4] Semantic Analysis (Pass 1 & 2)...\n";
        SemanticAnalyzer sema;
        sema.analyze(&program);

        std::cout << "[4/4] Code Generation...\n";
        
        // Feed a stringstream directly into the CodeGen constructor
        std::ostringstream cCodeStream;
        CodeGenVisitor codegen(cCodeStream);
        codegen.setKernelMode(targetKernel);
        codegen.generate(&program);
        std::string cCode = cCodeStream.str();

        // 3. Write unified intermediate C file
        std::ofstream outC(cFilename);
        if (!outC.is_open()) {
            std::cerr << "Failed to write intermediate file: " << cFilename << "\n";
            return 1;
        }
        outC << cCode;
        outC.close();

        // 4. Handle --emit-c (Stop here)
        if (emitC) {
            std::cout << "Success! Emitted unified C code to -> " << cFilename << "\n";
            return 0;
        }

        // 5. Invoke GCC
        std::string gccCmd = "gcc ";
        if (targetKernel) {
            // OS Kernel constraints: strictly GNU99, no SSE/Float registers, ignore integer casts for port I/O
            gccCmd += "-c -I. -mgeneral-regs-only -Wno-error=int-conversion -Wno-int-conversion -Wno-pointer-to-int-cast -std=gnu99 ";
        } else {
            // Host Windows/Linux constraints: standard warnings
            gccCmd += "-Wall -Wextra ";
        }
        
        gccCmd += cFilename + " -o " + outputFile;

        std::cout << "[GCC] " << gccCmd << "\n";
        int result = std::system(gccCmd.c_str());

        if (result == 0) {
            // 6. Cleanup intermediate file on success
            std::remove(cFilename.c_str());
            std::cout << "Success! Executable forged -> " << outputFile << "\n";
        } else {
            std::cerr << "\n[FATAL] GCC backend compilation failed. Unified C code left intact at '" << cFilename << "' for debugging.\n";
            return result;
        }

    } catch (const std::exception& e) {
        std::cerr << "\nCOMPILATION FAILED:\n" << e.what() << "\n";
        return 1;
    }

    return 0;
}