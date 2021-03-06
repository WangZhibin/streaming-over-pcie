/*******************************************************************
 * This is a test program for the C interface of the 
 * pciDriver library adaptaded to the VC707 Dev Board.
 * 
 * $Revision: 1.2 $
 * $Date: 2013-02-25 $
 * 
 *******************************************************************/

/*******************************************************************
 * Change History:
 * 
 * $Log: not supported by cvs2svn $
 * Revision 1.1  2006-10-16 16:56:56  marcus
 * Initial version. Tests the C interface of the library.
 *
 *******************************************************************/

#include "pciedma.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
 #include <stdarg.h>



//#define DEBUG

/* For Debugging */
#ifdef DEBUG
  #define PRINT	print
#else
  #define PRINT(...) ((void)0)
#endif

/* GLOBAL VARIABLES */
/*----------------------*/
unsigned int tail_desc_snd, tail_desc_recv;
pd_umem_t um_snd, um_recv;
void *bar;
int dest_tx; // Stream destination

void print(const char *fmt, ...)
{
 va_list args;
 char str[128];
 
 va_start(args,fmt);
 vsprintf(str,fmt,args);
 va_end(args);
 
 printf("%s",str);
}

/* transaction FROM device goes as follows :
 * 1. allocate buffer in kernel space for the data to transfer
 * 2. map the buffer to bus address space
 * 3. setup an entry in avalon-mm-to-pcie addr translation table
 * 4. setup dma transaction: src, dest, length
 * 5. init completion variable
 * 6. enable irq in the pcie bridge
 * 7. fire the transfer, wait for completion
 * 8. in the interrupt, determine
 *    the avalon interrupt # and complete appropriate compl var.
 * 9. disable irq in the pcie bridge
 * a. confirm the dma status
 * b. unmap dma buffer
 * c. copy the data from kernel space to user space
 * d. free kernel-space buffer
 */

/* transaction TO device goes as follows :
 * 1. allocate buffer in kernel space for the data to transfer
 * 2. copy the data from user space to kernel space
 * 3. map the buffer to bus address space
 * 4. init completion variable
 * 5. setup an entry in avalon-mm-to-pcie addr translation table
 * 6. setup dma transaction: src, dest, length
 * 7. enable irq in the pcie bridge
 * 8. fire the transfer, wait for completion
 * 9. in the interrupt, determine
 *    the avalon interrupt # and complete appropriate compl var.
 * a. disable irq in the pcie bridge
 * b. confirm the dma status
 * c. unmap dma buffer
 * d. free kernel-space buffer
 */



void dumpBRAM(void *bar)
{
 int i;
 
 PRINT("Dumping BRAM contents\n"); 
 
 for(i=0;i<4096/4;i++){
  printf("%08x ",((unsigned int*)bar)[BRAM_INDEX + i]); 
 }
}

void resetBRAM(void *bar)
{
 int i;
 
 PRINT("Resetting BRAM contents\n");
 
  for(i=0;i<4096/4;i++){
  ((unsigned int*)bar)[BRAM_INDEX + i] = 0; 
 }
}

/* Returns the AXI address corresponding to the PCIE space address
 * pci_addr - The address referred to the AXI side
 */
unsigned int getAXIaddr(unsigned int pci_addr)
{
 return pci_addr + PCIE2AXI; 
}

/* Returns the PCIE address corresponding to the AXI space address
 * pci_addr - The address referred to the PCIE side
 */
unsigned int getPCIEaddr(unsigned int axi_addr)
{
 return axi_addr - PCIE2AXI;
}


/* Set address translation from AXI to PCIe address space, by finding a base that makes all SG Entries addressable by the AXI bus 
* umem - user memory mapped to the PCI device
* umem_tr - user memory with addresses referred to the AXI side
* base_ptr - base address for the AXI2PCIE translation
*/
int addrTranslation(pd_umem_t *umem, pd_umem_t **umem_tr, unsigned int *base_ptr)
{
  unsigned int base_addr;
  int i;

  if (umem->nents == 0){
     PRINT("Error: SG list is empty\n");
     return -1;
  } 

  // Create a umem structure with translated addresses
  *umem_tr = (pd_umem_t*)malloc(sizeof(pd_umem_t));
  if(*umem_tr == NULL){
     PRINT("Error: Could not malloc umem structure\n");
     return(-1);
  }

  (*umem_tr)->vma = umem->vma;
  (*umem_tr)->size = umem->size;
  (*umem_tr)->handle_id = umem->handle_id;
  (*umem_tr)->nents = umem->nents;
  (*umem_tr)->pci_handle = umem->pci_handle;
  
  (*umem_tr)->sg = (pd_umem_sgentry_t*)malloc(sizeof(pd_umem_sgentry_t)*umem->nents);
  if((*umem_tr)->sg == NULL){
     PRINT("Error: Could not malloc sgentry structure\n");
     return(-1);
  }

  // Get base address from first SG descriptor
  base_addr = (umem->sg[0]).addr & AXI2PCIE_MASK;

// All the remaining descriptors must fit within the addressable space of the AXI bus...
  for(i = 0; i < umem->nents; i++){
	if((umem->sg[i]).addr & AXI2PCIE_MASK != base_addr){
	   PRINT("Error: Could not map descriptors into addressable range of the AXI bus\n");
	   return(-1);
	}

	// Save translated address
	((*umem_tr)->sg[i]).addr = (umem->sg[i]).addr - base_addr;
	((*umem_tr)->sg[i]).size = (umem->sg[i]).size;
  }

  *base_ptr = base_addr;
  return(0);  
}

/* Setup an entry in the AXI2PCIEbar register
 * base_ptr - pointer to the base of the memory range that will hold the transferred data
 * bar_ptr - pointer to the mapped BAR0 of the PCIE core
 */
int setAXI2PCIEbar(unsigned int base_ptr, void *bar_ptr)
{
  unsigned int prev_ptr;
  
  prev_ptr = ((unsigned int*)bar_ptr)[AXIBAR2PCIEBAR0/4];
  // Print current AXI2PCIE Vector for debugging
  PRINT("Previous AXI2PCIE Vector: %08x\n",prev_ptr);
  
  if(prev_ptr == base_ptr)
    return 0;
  
  // Set new AXI2PCIE Vector
  PRINT("Setting AXI2PCIE Vector to: %08x\n",base_ptr);
  ((unsigned int*)bar_ptr)[AXIBAR2PCIEBAR0/4] = base_ptr;
  
  // Check if vector changed successfuly
  if(base_ptr != ((unsigned int*)bar_ptr)[AXIBAR2PCIEBAR0/4]){
      PRINT("AXI2PCIE Vector configuration failed.\n");
      return -1;
  }
  
  return 0;
}

int checkDMAerrors(void *bar_ptr, unsigned int STATUSREG)
{
 unsigned int status_reg;
 int ret = 0; 
 
 status_reg = ((unsigned int*)bar_ptr)[DMA_INDEX + (STATUSREG/4)];
 
 if(status_reg & DMASR_DMAINTERR){
   PRINT("Error: DMA Internal Error\n");
   ret = -1; 
 }
 if(status_reg & DMASR_SGSLVERR){
   PRINT("Error: DMA Slave Error\n");
  ret = -1; 
 }
 if(status_reg & DMASR_DMADECERR){
   PRINT("Error: DMA Decode Error\n");
  ret = -1; 
 }
 if(status_reg & DMASR_SGINTERR){
   PRINT("Error: DMA Scatter Gather Internal Error\n");
  ret = -1; 
 }
 if(status_reg & DMASR_SGSLVERR){
   PRINT("Error: DMA Scatter Gather Slave Error\n"); ret = -1; 
 }
 if(status_reg & DMASR_SGDECERR){
   PRINT("Error: DMA Scatter Gather Decode Error\n");
  ret = -1; 
 }
 
 if(ret == -1){
   //dumpBRAM(bar_ptr);
   resetBRAM(bar_ptr); 
 }
  return ret;
}

void MM2Sreset(void *bar_ptr)
{
  unsigned int read_reset;
  
 // Set soft reset bit
 ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] =  ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] | DMACR_RESET;
 
 // Check if reset is done
 do{
   read_reset = ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] & DMACR_RESET;
   usleep(10); // Sleep for 10 us
   
}
 while(read_reset != 0);
  
}

void S2MMreset(void *bar_ptr)
{
  unsigned int read_reset;
  
 // Set soft reset bit
 ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] =  ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] | DMACR_RESET;
 
 // Check if reset is done
 do{
   read_reset = ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] & DMACR_RESET;
   usleep(10); // Sleep for 10 us
   
}
 while(read_reset != 0);
  
}

/* Write a DMA descriptor to BRAM
 * bar_ptr - pointer to the mapped BAR0 of the PCIE core
 * cur_desc - location of BRAM on which the current descriptor must be written to
 * first - first descriptor in the chain
 * last - indicates if it is the last descriptor in the chain
 * buff_addr - address of the buffer associated with this descriptor
 * buff_len - length in bytes of the buffer
//  * istx - TX descriptor (1) or RX descriptor (0)
 */
void write_desc(void *bar_ptr, unsigned int cur_desc, int first, int last, unsigned int buff_addr, unsigned int buff_len, int istx)
{
  unsigned int write_nxt, ctl_reg, multichannel_reg, stride_reg;
  
  if(!last)
  	write_nxt = (cur_desc + 0x40); // Write next descriptor pointer (absolute address)
  else
	write_nxt = cur_desc;

  write_nxt = getAXIaddr(write_nxt);
  ((unsigned int*)bar_ptr)[(cur_desc/4) + (NXTDESC/4)] = write_nxt;
  
  ((unsigned int*)bar_ptr)[(cur_desc/4) + (BUFFER_ADDRESS/4)] = buff_addr;
  
  
  // Changes for multichannel and 2D patterns support
  //-------------------------------------------------
  
  // Write Multichannel Register
  multichannel_reg = 0;
  
  if (istx){
    multichannel_reg = multichannel_reg | (dest_tx & TDEST);
    multichannel_reg = multichannel_reg | ((dest_tx << TID_SHIFT) & TID);
    //multichannel_reg = multichannel_reg | ((0 << TUSER_SHIFT) & TUSER);
    multichannel_reg = multichannel_reg | ((ARCACHE_DEF << ARCACHE_SHIFT) & ARCACHE);
    //multichannel_reg = multichannel_reg | ((0 << ARUSER_SHIFT) & ARUSER);
  }
  else{
    multichannel_reg = multichannel_reg | ((AWCACHE_DEF << AWCACHE_SHIFT) & AWCACHE);
    //multichannel_reg = multichannel_reg | ((0 << AWUSER_SHIFT) & AWUSER);
  }
  
  ((unsigned int*)bar_ptr)[(cur_desc/4) + (MC_CTL/4)] = multichannel_reg;

  // Write Stride Control Register
  stride_reg = 0;
  
  stride_reg = (0 & STRIDE); // Set up stride
  stride_reg = stride_reg | ((1 << VSIZE_SHIFT) & VSIZE);
  
  ((unsigned int*)bar_ptr)[(cur_desc/4) + (STRIDE_CTL/4)] = stride_reg;
  //-------------------------------------------------
  
  
  // Write Control Register
  ctl_reg = 0;
  ctl_reg = buff_len;
  if(first)
    ctl_reg = ctl_reg | CONTROL_TXSOF; // Set Start of Frame to 1
  if(last)
    ctl_reg = ctl_reg | CONTROL_TXEOF; // and End of Frame to 1 to signal the end of the frame
  
  ((unsigned int*)bar_ptr)[(cur_desc/4) + (CONTROL/4)] = ctl_reg;
}

/* Setup SG Desriptors for a DMA transfer
 * 
 */
int setupSGDesc(pd_umem_t *umem, void *bar_ptr, unsigned int base_loc, unsigned int *tail_desc, int istx)
{
  unsigned int next_desc, buff_addr, buff_len;
  int i,last = 0;
  
  if(base_loc < BRAM_BASE || base_loc > BRAM_BASE + 0x1000){
      PRINT("Error: Base location for descriptor ring must be within the BRAM Adress Space\n");
      return -1;
  }
  
  if(base_loc % 0x40 != 0){
      PRINT("Error: Base location must be 16-word aligned\n");
      return -1;
  }
  
  PRINT("Number of SG entries: %d\n", umem->nents );
		
	// Create descriptors and write them to the BRAM memory
	next_desc = (unsigned int)base_loc;
	for(i=0;i<umem->nents;i++) {
		PRINT("Descriptor %d: %08x - %08x\n", i, (umem->sg[i]).addr, (umem->sg[i]).size);
		
		buff_addr = (umem->sg[i]).addr;
		buff_len = (umem->sg[i]).size;
		
		if(buff_len >= 0x800000){
		    PRINT("Error: Buffer is larger than 23 bits. Aborting\n");
		    return -1;
		}
		
		// Save the location of the tail descriptor
		if(i == (umem->nents - 1)){
		  *tail_desc = next_desc;
		  last = 1;
		}

                write_desc(bar_ptr, next_desc, (i==0), last, buff_addr, buff_len, istx);
		
		next_desc += + 0x40; // Next descriptor is placed 16 words after current one
	}

  return 0;  
}

int setupDMAsend(void *bar_ptr, unsigned int cur_desc)
{
  unsigned int read_status;
  
    //0. Check if DMA engine is still running
     if(!(((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMASR/4)] & DMASR_HALTED)){
	PRINT("Error: MM2S channel is still running. Current transfer will be aborted\n");
	//return -1;
    }
    
    // Stop the MM2S channel by setting run/stop bit to 0
    ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] =  ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] & ~DMACR_RS;
    
    //1. Write absolute address of the starting descriptor to the DMA controller
    ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_CURDESC/4)] = getAXIaddr(cur_desc);
    
    //2. Start the MM2S channel running by setting run/stop bit to 1
    ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] =  ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] | DMACR_RS;
    
    // DMASR.Halted bit should deassert
    do{
      read_status = ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMASR/4)] & DMASR_HALTED;
      usleep(10); // wait for 10 us
    }while(read_status != 0);
    
    PRINT("MM2S channel is running\n");
    
    //3. Optionally enable interrupts
    //((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] =  ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] | DMACR_IOC_IrqEn;
    //((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] =  ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] | DMACR_Err_IrqEn;

    
    // Write 0x1 to the IRQThreshold
    //((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] =  ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] & ~DMACR_IRQThreshold;
    //((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] =  ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] | (0x1 << 16);    
    
    return 0;
}

int setupDMArecv(void *bar_ptr, unsigned int cur_desc)
{
  unsigned int read_status;
  
    //0. Check if DMA engine is still running
     if(!(((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMASR/4)] & DMASR_HALTED)){
	PRINT("Error: S2MM channel is still running. Current transfer will be aborted\n");
	//return -1;
    }
    
    // Stop the S2MM channel by setting run/stop bit to 0
    ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] =  ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] & ~DMACR_RS;
    
    //1. Write absolute address of the starting descriptor to the DMA controller
    cur_desc = getAXIaddr(cur_desc);
    ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_CURDESC/4)] = cur_desc;
    PRINT("Setting current descriptor on S2MM to: %08x\n",cur_desc);
    
    //2. Start the S2MM channel running by setting run/stop bit to 1
    ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] =  ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] | DMACR_RS;
    
    // DMASR.Halted bit should deassert
    do{
      read_status = ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMASR/4)] & DMASR_HALTED;
      usleep(10); // sleep for 10 us
    }while(read_status != 0);
    
    PRINT("S2MM channel is running\n");
    
    //3. Optionally enable interrupts
    ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] =  ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] | DMACR_IOC_IrqEn;
    ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] =  ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] | DMACR_Err_IrqEn;

    // Write 0x1 to the IRQThreshold
    ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] =  ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] & ~DMACR_IRQThreshold;
    ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] =  ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] | (0x1 << 16);
    
    
    return 0;
}

void writeTailSend(void *bar_ptr, unsigned int tail_desc)
{
  //4. Write an absolute address to the tail descriptor register
    tail_desc = getAXIaddr(tail_desc);
    ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_TAILDESC/4)] = tail_desc;
    PRINT("Tail descriptor register MM2S: %08x\n",tail_desc);
}

void writeTailRecv(void *bar_ptr, unsigned int tail_desc)
{
    //4. Write an absolute address to the tail descriptor register
    tail_desc = getAXIaddr(tail_desc);
    ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_TAILDESC/4)] = tail_desc;
   
    PRINT("S2MM Tail descriptor register: %08x\n", tail_desc);
}


int checkSendCompletion(void *bar_ptr,unsigned int desc_base)
{
 unsigned int status_reg, next_desc, total_size;
  int i;
 
  //dumpBRAM(bar_ptr);

  // Check for errors
  if(checkDMAerrors(bar_ptr, MM2S_DMASR) < 0){
      PRINT("Resetting MM2S channel\n");
      MM2Sreset(bar_ptr);
      return -1;
  }
 
  PRINT("Current descriptor being worked on: %08x\n",((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_CURDESC/4)]);
 
   // Check if DMA Channel is Idle
  do{
    status_reg = ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMASR/4)] & DMASR_IDLE;
    usleep(1);
//	PRINT("status_reg: %08x\n");
  }while(status_reg != DMASR_IDLE);
    
  PRINT("MM2S channel is idle\n");
 

  // Check all transmitted descriptors
  next_desc = desc_base;
  total_size = 0;
  i = 0;
  while(1){
    status_reg = ((unsigned int*)bar_ptr)[(next_desc/4) + (STATUS/4)];
    
    if(status_reg & STATUS_CMPLT)
    	PRINT("Descriptor %d transfer completed successfuly\n",i);
    else{
    	PRINT("Error: Could not complete descriptor %d transfer\n",i);
	break;
    }

    total_size += status_reg & STATUS_TRANSF;

    // Erase status descriptor
    ((unsigned int*)bar_ptr)[(next_desc/4) + (STATUS/4)] = 0;

    if(next_desc == tail_desc_snd)
      break; // Reached end of descriptor chain
    
    next_desc = getPCIEaddr(((unsigned int*)bar_ptr)[(next_desc/4) + (NXTDESC/4)]);
    i++;

  }

     
  PRINT("Total bytes sent over %d descriptors: %d\n",i+1,total_size);

   // Stop the MM2S channel by setting run/stop bit to 0
    ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] =  ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMACR/4)] & ~DMACR_RS;
    
  return 0;
}

int checkRecvCompletion(void *bar_ptr,unsigned int desc_base)
{
  unsigned int status_reg, next_desc, total_size;
  int i; 
  
  // Check for errors
  if(checkDMAerrors(bar_ptr, S2MM_DMASR) < 0){
      PRINT("Resetting S2MM channel\n");
      S2MMreset(bar_ptr);
      return -1;
  }

  PRINT("Current descriptor being worked on: %08x\n",((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_CURDESC/4)]); 
  
    
  // Check if DMA Channel is Idle
  do{
    status_reg = ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMASR/4)] & DMASR_IDLE;
    usleep(1);
  }while(status_reg != DMASR_IDLE);
  
  PRINT("S2MM channel is idle\n");

  
 // Check all transmitted descriptors
  next_desc = desc_base;
  total_size = 0;
  i = 0;
  while(1){
    status_reg = ((unsigned int*)bar_ptr)[(next_desc/4) + (STATUS/4)];
    
    if(status_reg & STATUS_CMPLT)
    	PRINT("Descriptor %d transfer completed successfuly\n",i);
    else{
    	PRINT("Error: Could not complete descriptor %d transfer\n",i);
	break;
    }
    
    
    PRINT("S2MM RXEOF: %08x\n",status_reg & STATUS_RXEOF);  
    PRINT("S2MM RXSOF: %08x\n",status_reg & STATUS_RXSOF); 
    PRINT("S2MM TDEST: %08x\n",status_reg & TDEST);
    PRINT("S2MM TUSER: %08x\n",(status_reg >> TID_SHIFT) & TID);

    total_size += status_reg & STATUS_TRANSF;

    // Erase status descriptor
    ((unsigned int*)bar_ptr)[(next_desc/4) + (STATUS/4)] = 0;

    if(next_desc == tail_desc_recv)
      break; // Reached end of descriptor chain
    
    next_desc = getPCIEaddr(((unsigned int*)bar_ptr)[(next_desc/4) + (NXTDESC/4)]);
    i++;

  }
 
  PRINT("Total bytes sent over %d descriptors: %d\n",i+1,total_size);

  if(status_reg & STATUS_CMPLT)
    PRINT("Descriptor transfer completed successfuly\n");
  else{
    PRINT("Error: Could not complete descriptor transfer\n");
    return -1;
  } 
  
  // Stop the S2MM channel by setting run/stop bit to 0
    ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] =  ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMACR/4)] & ~DMACR_RS;
    
  return 0;
}

int waitIOC(pd_device_t *pdev, void *bar_ptr)
{
 unsigned int status_reg_s2mm, status_reg_mm2s;
 
 if( pd_waitForInterrupt(pdev, 0) < 0){
    PRINT("Error: Could not wait for interrupt\n");
    return -1;
 } 

 status_reg_mm2s = ((unsigned int*)bar_ptr)[DMA_INDEX + (MM2S_DMASR/4)];
 status_reg_s2mm = ((unsigned int*)bar_ptr)[DMA_INDEX + (S2MM_DMASR/4)];

 if(status_reg_mm2s & DMASR_IOC_IRQ)
   PRINT("Received interrupt for MM2S completion\n");
 if(status_reg_s2mm & DMASR_IOC_IRQ)
   PRINT("Received interrupt for S2MM completion\n");
 
 if(status_reg_mm2s & DMASR_ERR_IRQ)
   PRINT("Received interrupt on error for MM2S completion\n");
 if(status_reg_s2mm & DMASR_ERR_IRQ)
   PRINT("Received interrupt on error for S2MM completion\n");

 return 0;
  
}

/* Initializes the framework by mapping the BAR of the PCIE bridge and resetting the DMA engine; Should be called prior to anything else
 * Arguments: pdev - pcie device handler obtained from a successful call to open()
 * Returns: 0 on sucess, -1 otherwise
 */
int initDMA(pd_device_t *pdev)
{ 
 bar = pd_mapBAR(pdev,0);
 if (bar == NULL) {
   PRINT("Error: Could not map BAR0\n");
   return -1;
 }
 
 resetBRAM(bar);
 
  PRINT("Resetting DMA controller\n");
  // Reset DMA controller
  MM2Sreset(bar);
 
  return 0;
}

/* Prepares a MM2S DMA transfer by mapping the user memory into device space, translating addresses and writing the SG descriptors to BRAM
 * Arguments: pdev - pcie device handler
 * 	      user_buffer - pointer to user buffer that holds the data to transmit
 * 	      buf_size - size in bytes of the data to transmit
 * 	      str_dest - stream destination (slave address) 
 * Returns: 0 on sucess, -1 otherwise
 */
int setupSend(pd_device_t *pdev, void *user_buffer, unsigned int buf_size, int str_dest)
{
  pd_umem_t *umem_tr;
  unsigned int base_axi2pcie;
  
  int i;
  
  if(bar == NULL){
      PRINT("Error: BAR pointer is not initialized\n");
      return -1;
  }

  // Map user buffer into device space
  if(pd_mapUserMemory( pdev, user_buffer, buf_size, &um_snd) < 0){
      PRINT("Error: Could not allocate provided user buffer\n");
      return -1;
  }
  
  // Translate addresses to the AXI bus side
  if( addrTranslation(&um_snd, &umem_tr, &base_axi2pcie) < 0){
     PRINT("Error: Could not translate addresses for the AXI bus\n");
     return -1;
  }

  PRINT("Setting address translation\n");
  // Set Address translation in the PCIE Core
  if( setAXI2PCIEbar(base_axi2pcie, bar) < 0){
     PRINT("Error: Could not configure address translation on the PCIe core\n");
     return -1;
  }
  
   PRINT("Writing SG descriptors to BRAM\n");
   
   // Set stream destination
   dest_tx = str_dest;
   
  // Setup translated SG descriptors in the BRAM
  if(setupSGDesc(umem_tr,bar,BRAM_BASE,&tail_desc_snd,1) < 0){
      PRINT("Error: Could not setup DMA transfer\n");
      return -1;
  }
  
   PRINT("Setting up DMA send\n");  
   if(setupDMAsend(bar,BRAM_BASE) < 0){
      PRINT("Error: Could not setup DMA transfer\n");
      return -1;
   }
  
  free(umem_tr);
  
  return 0;
}

/* Prepares a S2MM DMA transfer by mapping the user memory into device space, translating addresses and writing the SG descriptors to BRAM
 * Arguments: pdev - pcie device handler
 * 	      user_buffer - pointer to user buffer that holds the data to transmit
 * 	      buf_size - size in bytes of the data to transmit
 * Returns: 0 on sucess, -1 otherwise
 */
int setupRecv(pd_device_t *pdev, void *user_buffer, unsigned int buf_size)
{
  pd_umem_t *umem_tr;
  unsigned int base_axi2pcie;
  
  int i;

  if(bar == NULL){
     PRINT("Error: BAR pointer is not initialized\n");
     return -1;
  }
  
  // Map user buffer into device space
  if(pd_mapUserMemory( pdev, user_buffer, buf_size, &um_recv) < 0){
      PRINT("Error: Could not allocate provided user buffer\n");
      return -1;
  }
  
  // Translate addresses to the AXI bus side
  if( addrTranslation(&um_recv, &umem_tr, &base_axi2pcie) < 0){
     PRINT("Error: Could not translate addresses for the AXI bus\n");
     return -1;
  }

  PRINT("Setting address translation\n");
  // Set Address translation in the PCIE Core
  if( setAXI2PCIEbar(base_axi2pcie, bar) < 0){
     PRINT("Error: Could not configure address translation on the PCIe core\n");
     return -1;
  }
  
   PRINT("Writing SG descriptors to BRAM\n");
  // Setup translated SG descriptors in the BRAM
  if(setupSGDesc(umem_tr,bar,BRAM_BASE+0x800,&tail_desc_recv,0) < 0){
      PRINT("Error: Could not setup DMA transfer\n");
      return -1;
  }
  
  PRINT("Setting up DMA receive\n");  
   if(setupDMArecv(bar,BRAM_BASE+0x800) < 0){
      PRINT("Error: Could not setup DMA receive\n");
      return -1;
   }
  
  free(umem_tr);

  
  return 0; 
}

/* Starts DMA MM2S transfer with the parameters set on the previous call to setupSend()
 * Arguments: blocking - 1 for interrupt mode, 0 for non blocking mode
 * Returns: 0 on success, -1 otherwise
 */
int startSend(pd_device_t *pdev, int blocking)
{
   writeTailSend(bar,tail_desc_snd);
   
   if(blocking){
     PRINT("Wait on interrupt\n");
     if(waitIOC(pdev,bar)<0){
	PRINT("Error: Wait on interrupt failed\n");
     return -1;
     }
   }
   
   return 0;  
}


/* Starts DMA S2MM transfer with the parameters set on the previous call to setupRecv()
 * Arguments: blocking - 1 for interrupt mode, 0 for non blocking mode
 * Returns: 0 on success, -1 otherwise
 */
int startRecv(pd_device_t *pdev, int blocking)
{
  writeTailRecv(bar,tail_desc_recv);
  
  if(blocking){
     PRINT("Wait on interrupt\n");
     if(waitIOC(pdev,bar)<0){
	PRINT("Error: Wait on interrupt failed\n");
     return -1;
     }
   }
   
   return 0;  
}

/* Checks if the DMA MM2S transfer is complete and there were no errors; this function should be always called before starting other transfer
 * Arguments: 
 * Returns: 0 on success, -1 otherwise
 */
int checkSend()
{
  // Check DMA status and completion
  if(checkSendCompletion(bar,BRAM_BASE) < 0){
     PRINT("Error: DMA transfer failed\n");
     return -1;
  }
  
  return 0;
}

/* Checks if the DMA S2MM transfer is complete and there were no errors; 
 * This function should be always called before using the receive buffer because it performs necessary syncing between the user buffer and kernel buffer
 * Arguments: 
 * Returns: 0 on success, -1 otherwise
 */
int checkRecv()
{
  // Check DMA status and completion
  if(checkRecvCompletion(bar,BRAM_BASE+0x800) < 0){
     PRINT("Error: DMA transfer failed\n");
     return -1;
  }
  
  if(pd_syncUserMemory(&um_recv, PD_DIR_FROMDEVICE) <0){
      PRINT("Error: Could not sync user memory\n");
      return -1;
  }
  
  return 0;
}

/* Frees the user memory mapping to bus device space previously done for MM2S transfer
 * Arguments: pdev - pcie device handler
 * Returns: 0 on sucess, -1 otherwise
 */
int freeSend(pd_device_t *pdev)
{
   if(pd_unmapUserMemory( &um_snd ) < 0){
      PRINT("Error: Could not unmap user memory\n");
      return -1;
   }  
   
   return 0;
}

/* Frees the user memory mapping to bus device space previously done for S2MM transfer
 * Arguments: pdev - pcie device handler
 * Returns: 0 on sucess, -1 otherwise
 */
int freeRecv(pd_device_t *pdev)
{
   if(pd_unmapUserMemory( &um_recv ) < 0){
      PRINT("Error: Could not unmap user memory\n");
      return -1;
   }  
   
   return 0;
}

/* Stops the framework by unmpaping the pcie BAR
 * Arguments: pdev - pcie device handler obtained from a successful call to open()
 * Returns: 0 on sucess, -1 otherwise
 */
int stopDMA(pd_device_t *pdev)
{ 
  if(pd_unmapBAR(pdev,0,bar)<0){
     PRINT("Error: Could not unmpap BAR0\n");
     return -1;
   }
 
  return 0;
}








