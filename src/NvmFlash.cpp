///////////////////////////////////////////////////////////////////////////////
// BOSSA
//
// Copyright (C) 2011-2012 ShumaTech http://www.shumatech.com/
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
///////////////////////////////////////////////////////////////////////////////

#include <stdint.h>
#include <malloc.h>
#include <memory>
#include <iostream>
#include <exception>

#include "Samba.h"
#include "WordCopyApplet.h"
#include "NvmFlash.h"

using namespace std;


//NVM User row in flash, 64 bytes in length
#define NVMCTRL_USER_ROW 0x804000

//NVM System control brown out register.
#define SYSCTRL_BOD33_REG (0x40000800 + 0x34) //SYSCTRL base address + BOD33 reg offset
#define SYSCTRL_STATUS_REG_ENABLE_BIT 0x2
//The _regs parameter to this class is the module base address.
//redefined here with a more appropriate name
#define MODULE_BASE_ADDR _regs

//The base address of the NVM module in 
//main memory + offset to the CTRLA register
#define NVM_CTRLA_REG (MODULE_BASE_ADDR+0x00)

//The NVM register that stores lock status
#define NVM_LOCK_REG (MODULE_BASE_ADDR+0x20)

//The interrupt status register
#define NVM_INT_STATUS (MODULE_BASE_ADDR+0x14)

//NVM input register to some of the 
//CMDEX commands.
#define ADDR_REG  (MODULE_BASE_ADDR+0x1c)

//NVM STATUS register
#define STATUS (MODULE_BASE_ADDR+0x18)

//CMDEX should be 0xA5 to execute any command 
//on the NVM controller's APB bus.
#define CMDEX 0xa500   

//List of NVM Commands.//as per datasheet prefix CMDEX
#define CMD_LOCK_REGION   (CMDEX | 0x0040)
#define CMD_UNLOCK_REGION (CMDEX | 0x0041)
#define CMD_ERASE_ROW     (CMDEX | 0x0002)
#define CMD_SET_SECURITY_BIT (CMDEX | 0x0045)


//Just for readability
#define FOUR_PAGES 4

//Size of a page should be computed based on the input
#define PAGE_SIZE_IN_BYTES (_size/_pages) 
//Size of the samba bootloader in any configuration
#define BOOTLOADER_SIZE_IN_BYTES 8192 

//This is the row size which is a standard number for all SAMD21
#define ROW_SIZE FOUR_PAGES 

/* This class is designed specifically for M0+ architecture in mind */
NvmFlash::NvmFlash(Samba& samba,
             const std::string& name,
             uint32_t addr,
             uint32_t pages,
             uint32_t size,
             uint32_t planes,
             uint32_t lockRegions,
             uint32_t user,
             uint32_t stack,
             uint32_t regs,
             bool canBrownout)
    : Flash(samba, name, addr, pages, size, planes, lockRegions, user, stack),
    _regs(regs), _canBrownout(canBrownout), _eraseAuto(true)
{
   ///Upon power up the NVM controller goes through a power up sequence. 
   //During this time, access to the NVM controller is halted. Upon power up the
   //the NVM controller is operational without any need for user configuration.
}

NvmFlash::~NvmFlash() 
{

}

void
NvmFlash::eraseAll()
{
    //Leave the first 8KB, where samba resides, erase the rest.
    //Row is a concept used for convinence. When writing you have to write 
    //page(s). When erasing you have to erase row(s).
    uint32_t total_rows = _pages/ROW_SIZE;
    uint32_t boot_rows = (BOOTLOADER_SIZE_IN_BYTES/PAGE_SIZE_IN_BYTES)/ROW_SIZE;

    for(uint32_t row=boot_rows+10;row<=total_rows-10;row++)
    {
        uint32_t addr_in_flash = _addr + (row * ROW_SIZE * PAGE_SIZE_IN_BYTES);
	// the address is byte address, so convert it to word address.
	addr_in_flash = (addr_in_flash / 4);

        while(!nvm_is_ready())
        {
            std::cout<<endl<<"Waiting ..... ";
        }

        _samba.writeWord(ADDR_REG, addr_in_flash) ;
        _samba.writeWord(NVM_CTRLA_REG, CMD_ERASE_ROW);
    }
}


bool
NvmFlash::nvm_is_ready()
{
    uint8_t int_flag = _samba.readByte(NVM_INT_STATUS)&0x1;//Read the ready bit
    return int_flag == 1;
}

void 
NvmFlash::eraseAuto(bool enable)
{
    _eraseAuto = enable;
}


bool 
NvmFlash::isLocked()
{
   return getLockRegion(0);
}

///Returns true if locked, false otherwise.
bool 
NvmFlash::getLockRegion(uint32_t region)
{
    if(region >= _lockRegions)
        throw FlashRegionError();

    uint32_t value = _samba.readWord(NVM_LOCK_REG); 
    return ((value & (1 << region)) == 0); //Read the bit corresponding to the region number, if it's 0 -> locked, 1 -> unlocked, 
}

void 
NvmFlash::setLockRegion(uint32_t region, bool enable)
{
   if(region >= _lockRegions)
     throw FlashRegionError();
   
   if(enable != getLockRegion(region))
   {
       if(enable)
       {
	   //To lock a region you have to pass an address to the
           //ADDR register, and then execute "lock region" cmd 
           //on the NVM controller.
	   uint32_t addr_to_lock = getAddressByRegion(region);
	   //addr_to_lock = addr_to_lock & 0x1fffff;
           while(!nvm_is_ready());
 	   _samba.writeWord(ADDR_REG, addr_to_lock);
           while(!nvm_is_ready());
           _samba.writeWord(NVM_CTRLA_REG, CMD_LOCK_REGION);
       }    
       else
       {
	   uint32_t addr_to_unlock = getAddressByRegion(region);
	   addr_to_unlock = addr_to_unlock & 0x1fffff; 
	   while(!nvm_is_ready());
 	   _samba.writeWord(ADDR_REG, addr_to_unlock);
	   while(!nvm_is_ready());
           _samba.writeWord(NVM_CTRLA_REG, CMD_UNLOCK_REGION);
       }
   }
}


bool 
NvmFlash::getSecurity()
{
    //Read status register and take only the LSB 16 bits
    while(!nvm_is_ready());
    uint16_t status_reg_value = _samba.readWord(STATUS) & 0xffff;
    //If the 8th bit is 1 then security bit is set, else unset.
    return (((status_reg_value >> 8) & 0x1) == 1);
}

void 
NvmFlash::setSecurity()
{
    if(!getSecurity()) //If security bit is not set
    {

        while(!nvm_is_ready());
        _samba.writeWord(NVM_CTRLA_REG, CMD_SET_SECURITY_BIT);	
	if(!getSecurity())
 	    throw NvmFlashCmdError("Error when setting security bit");
    }
}

//Enable/disable the Bod mechanism. The values are lost on target reset.
void 
NvmFlash::setBod(bool enable)
{
    uint32_t bod33_ctrl_reg = _samba.readWord(SYSCTRL_BOD33_REG);

    if(enable)
    {
        bod33_ctrl_reg |= SYSCTRL_STATUS_REG_ENABLE_BIT;//Enable the bod control
        _samba.writeWord(SYSCTRL_BOD33_REG, bod33_ctrl_reg);
    }
    else
    {
       bod33_ctrl_reg &= 0xfffffffd;//Negate just the STATUS_REG bit.
       _samba.writeWord(SYSCTRL_BOD33_REG, bod33_ctrl_reg);
    }
}

bool 
NvmFlash::getBod()
{
    uint32_t value = _samba.readWord(SYSCTRL_BOD33_REG);
    return (((value & SYSCTRL_STATUS_REG_ENABLE_BIT) >> 1) == 0x1); //If Bit 1 of the BOD33 register is 1, then it's enabled
}

bool 
NvmFlash::getBor()
{
    throw NvmFlashCmdError("BOR not supported in this target");
}

void 
NvmFlash::setBor(bool enable)
{
    throw NvmFlashCmdError("BOR not supported in this target");
}


bool 
NvmFlash::getBootFlash()
{
    //Always boots from flash. No ROM boot available.
    return true;
}

void 
NvmFlash::setBootFlash(bool enable)
{
    //Boot to flash is the only supported option. Other means are not possible with this device.
    throw BootFlashError();
}

void 
NvmFlash::loadBuffer(const uint8_t* data)
{

}

void 
NvmFlash::writePage(uint32_t page)
{
    if (page >= _pages)
        throw FlashPageError();
}

void 
NvmFlash::readPage(uint32_t page, uint8_t* buf)
{
    if(page >= _pages)
        throw FlashPageError();
    
    for(uint32_t i=0;i<8;i++)
        buf[i] = i;

    //Convert page number into physical address. flash_base_address + page.no * page_size
    uint32_t addr = _addr + (page * PAGE_SIZE_IN_BYTES);
    _samba.read(addr, buf, PAGE_SIZE_IN_BYTES);
    uint32_t number;
    for(int j=0;j<16;j++)
    {
        number = 0;
        for(int i = 0;i <4;i++)
        {
           number |= buf[i] << (8*i);
        }
    	printf("\n%x : %x",addr + (j*4), number);
    }
}

///Returns the start address of a specified region number
///based on the flash specifications. The returned address is 
///word address (not byte address).

uint32_t 
NvmFlash::getAddressByRegion(uint32_t region_num)
{
    if(region_num >= _lockRegions)
        throw FlashRegionError();
    
    uint32_t size_of_region = _size/_lockRegions; //Flash Size / no of lock regions
    uint32_t addr = address() + (region_num * size_of_region);
    addr = addr / 4; //Convert byte address to word address;
    return addr;
}

