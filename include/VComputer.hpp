#pragma once
/**
 * Trillek Virtual Computer - VComputer.hpp
 * Virtual Computer core
 */

#include "Types.hpp"

#include "ICPU.hpp"
#include "IDevice.hpp"
#include "AddrListener.hpp"
#include "EnumAndCtrlBlk.hpp"
#include "Timer.hpp"
#include "RNG.hpp"
#include "RTC.hpp"

#include <map>
#include <memory>
#include <cassert>

namespace vm {
  using namespace vm::cpu;

  const unsigned MAX_N_DEVICES    = 32;         /// Max number of devices attached
  const std::size_t MAX_ROM_SIZE  = 32*1024;    /// Max ROM size
  const std::size_t MAX_RAM_SIZE  = 1024*1024;  /// Max RAM size

  const unsigned EnumCtrlBlkSize  = 20;         /// Enumeration and Control registers blk size

  const unsigned BaseClock  = 1000000;          /// Computer Base Clock rate

  class EnumAndCtrlBlk;

  class VComputer {
    public:

      /**
       * Creates a Virtual Computer
       * @param ram_size RAM size in BYTES
       */
      VComputer (std::size_t ram_size = 128*1024);

      ~VComputer ();

      /**
       * Sets the CPU of the computer
       * @param cpu New CPU in the computer
       */
      void SetCPU (std::unique_ptr<ICPU> cpu);

      /**
       * Removes the CPU of the computer
       * @return Returns the ICPU
       */
      std::unique_ptr<ICPU> RmCPU ();

      /**
       * Adds a Device to a slot
       * @param slot Were plug the device
       * @param dev The device to be pluged in the slot
       * @return False if the slot have a device or the slot is invalid.
       */
      bool AddDevice (unsigned slot , std::shared_ptr<IDevice> dev);

      /**
       * Gets the Device plugged in the slot
       */
      std::shared_ptr<IDevice> GetDevice (unsigned slot);

      /**
       * Remove a device from a slot
       * @param slot Slot were unplug the device
       */
      void RmDevice (unsigned slot);

      /**
       * CPU clock speed in Hz
       */
      unsigned CPUClock() const;

      /**
       * Writes a copy of CPU state in a chunk of memory pointer by ptr.
       * @param ptr Pointer were to write
       * @param size Size of the chunk of memory were can write. If is
       * sucesfull, it will be set to the size of the write data.
       */
      void GetState (void* ptr, std::size_t size) const;

      /**
       * Gets a pointer were is stored the ROM data
       * @param *rom Ptr to the ROM data
       * @param rom_size Size of the ROM data that must be less or equal to 32KiB. Big sizes will be ignored
       */
      void SetROM (const byte_t* rom, std::size_t rom_size);

      /**
       * Resets the virtual machine (but not clears RAM!)
       */
      void Reset();

      /**
       * Executes one instruction
       * @param delta Number of seconds since the last call
       * @return number of base clock ticks needed
       */
      unsigned Step( const double delta = 0);

      /**
       * Executes N clock ticks
       * @param n nubmer of base clock ticks, by default 1
       * @param delta Number of seconds since the last call
       */
      void Tick( unsigned n=1, const double delta = 0) {
        assert(n >0);
        unsigned dev_ticks = n / 10; // Devices clock is at 100 KHz
        if (cpu) {
          unsigned cpu_ticks = n / (BaseClock / cpu->Clock());

          cpu->Tick(cpu_ticks);
          pit.Tick(dev_ticks, delta);

          word_t msg;
          bool interrupted = pit.DoesInterrupt(msg); // Highest priority interrupt
          if (interrupted) {
            if (cpu->SendInterrupt(msg)) { // Send the interrupt to the CPU
              pit.IACK();
            }
          }

          for (std::size_t i=0; i < MAX_N_DEVICES; i++) {
            if (! std::get<0>(devices[i])) {
              continue; // Slot without device
            }

            // Does the sync job
            if ( std::get<0>(devices[i])->IsSyncDev()) {
              std::get<0>(devices[i])->Tick(dev_ticks, delta);
            }

            // Try to get the highest priority interrupt
            if (! interrupted && std::get<0>(devices[i])->DoesInterrupt(msg) ) {
              interrupted = true;
              if (cpu->SendInterrupt(msg)) { // Send the interrupt to the CPU
                std::get<0>(devices[i])->IACK(); // Informs to the device that his interrupt has been accepted by the CPU
              }
            }
          }

        }
      }

      byte_t ReadB (dword_t addr) const {
        addr = addr & 0x00FFFFFF; // We use only 24 bit addresses

        if (!(addr & 0xF00000 )) { // RAM address (0x000000-0x0FFFFF)
          return ram[addr];
        }

        if ((addr & 0xFF0000) == 0x100000 ) { // ROM (0x100000-0x10FFFF)
          return rom[addr & 0x00FFFF];
        }

        Range r(addr);
        auto search = listeners.find(r);
        if (search != listeners.end()) {
          return search->second->ReadB(addr);
        }

        return 0;
      }

      word_t ReadW (dword_t addr) const {
        addr = addr & 0x00FFFFFF; // We use only 24 bit addresses
        size_t tmp;

        if (!(addr & 0xF00000 )) { // RAM address
          tmp = ((size_t)ram) + addr;
          return ((word_t*)tmp)[0];
        }

        if ((addr & 0xFF0000) == 0x100000 ) { // ROM (0x100000-0x10FFFF)
          addr &= 0x00FFFF; // Dirty tricks with pointers
          tmp = ((size_t)rom) + addr;
          return ((word_t*)tmp)[0];
        }

        Range r(addr);
        auto search = listeners.find(r);
        if (search != listeners.end()) {
          return search->second->ReadW(addr);
        }

        return 0;
      }

      dword_t ReadDW (dword_t addr) const {
        addr = addr & 0x00FFFFFF; // We use only 24 bit addresses
        size_t tmp;

        if (!(addr & 0xF00000 )) { // RAM address
          tmp = ((size_t)ram) + addr;
          return ((dword_t*)tmp)[0];
        }

        if ((addr & 0xFF0000) == 0x100000 ) { // ROM (0x100000-0x10FFFF)
          addr &= 0x00FFFF; // Dirty tricks with pointers
          tmp = ((size_t)rom) + addr;
          return ((dword_t*)tmp)[0];
        }

        Range r(addr);
        auto search = listeners.find(r);
        if (search != listeners.end()) {
          return search->second->ReadDW(addr);
        }

        return 0;
      }

      void WriteB (dword_t addr, byte_t val) {
        addr = addr & 0x00FFFFFF; // We use only 24 bit addresses

        if (addr < ram_size) { // RAM address
          ram[addr] = val;
        }

        Range r(addr);
        auto search = listeners.find(r);
        if (search != listeners.end()) {
          search->second->WriteB(addr, val);
        }

      }

      void WriteW (dword_t addr, word_t val) {
        size_t tmp;
        addr = addr & 0x00FFFFFF; // We use only 24 bit addresses

        if (addr < ram_size-1 ) { // RAM address
          tmp = ((size_t)ram) + addr;
          ((word_t*)tmp)[0] = val;
        }
        // TODO What hapens when there is a write that falls half in RAM and
        // half outside ?
        // I actually forbid these cases to avoid buffer overun, but should be
        // allowed and only use the apropiate portion of the data in the RAM.

        Range r(addr);
        auto search = listeners.find(r);
        if (search != listeners.end()) {
          search->second->WriteW(addr, val);
        }

      }

      void WriteDW (dword_t addr, dword_t val) {
        size_t tmp;
        addr = addr & 0x00FFFFFF; // We use only 24 bit addresses

        if (addr < ram_size-3 ) { // RAM address
          tmp = ((size_t)ram) + addr;
          ((dword_t*)tmp)[0] = val;
        }
        // TODO What hapens when there is a write that falls half in RAM and
        // half outside ?

        Range r(addr);
        auto search = listeners.find(r);
        if (search != listeners.end()) {
          search->second->WriteDW(addr, val);
        }

      }

      /**
       * Adds an AddrListener to the computer
       * @param range Range of addresses that the listerner listens
       * @param listener AddrListener using these range
       * @return And ID oif the listener or -1 if can't add the listener
       */
      int32_t AddAddrListener (const Range& range, AddrListener* listener);

      /**
       * Removes an AddrListener from the computer
       * @param id ID of the address listener to remove (ID from AddAddrListener)
       * @return True if can find these listener and remove it.
       */
      bool RmAddrListener (int32_t id);

      /**
       * Sizeo of the RAM in bytes
       */
      std::size_t RamSize() const {
        return ram_size;
      }

      /**
       * Returns a pointer to the RAM for reading raw values from it
       * Use only for GetState methods or dump a snapshot of the computer state
       */
      const byte_t* Ram() const {
        return ram;
      }

      /**
       * Returns a pointer to the RAM for writing raw values to it
       * Use only for SetState methods or load a snapshot of the computer state
       */
      byte_t* Ram() {
        return ram;
      }

    private:

      byte_t* ram;            /// Computer RAM
      const byte_t* rom;      /// Computer ROM chip (could be shared between some VComputers)
      std::size_t ram_size;   /// Computer RAM size
      std::size_t rom_size;   /// Computer ROM size

      std::unique_ptr<ICPU> cpu;                /// Virtual CPU

      device_t devices[MAX_N_DEVICES];          /// Devices atached to the virtual computer

      std::map<Range, AddrListener*> listeners; /// Container of AddrListeners

      Timer pit;  /// Programable Interval Timer
      RNG rng; /// Random Number Generator
      RTC rtc; /// Real Time Clock
  };


} // End of namespace vm

