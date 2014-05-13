#include "audio-vs1053b.h"

#define AUDIO_CHUNK_SIZE 32
#define SPI_PORT 0

typedef struct AudioBuffer{
  struct AudioBuffer *next;
  uint8_t *tx_buf;
  int32_t remaining_bytes;
  uint8_t chip_select;
  uint8_t dreq;
  uint8_t interrupt;
  uint16_t stream_id;
} AudioBuffer;

// Callbacks for SPI and GPIO Interrupt
void _audio_spi_callback();
void _audio_continue_spi();

// Linked List queueing methods
void _queue_buffer(AudioBuffer *buffer);
void _shift_buffer();

// Method to start listening for DREQ pulled low
void _audio_watch_dreq();
void _audio_emit_completion(uint16_t stream_id);
uint16_t _generate_stream_id();

AudioBuffer *operating_buf = NULL;

uint16_t master_id_gen = 0;

void _queue_buffer(AudioBuffer *buffer) {

  AudioBuffer **next = &operating_buf;
  // If we don't have an operating buffer already
  if (!(*next)) {
    TM_DEBUG("There is nothing else in the queue, starting now.");
    // Assign this buffer to the head
    *next = buffer;

    // Start watching for a low DREQ
    _audio_watch_dreq();
  }

  // If we do already have an operating buf
  else {
    TM_DEBUG("Already items in the queue, placing for now.");
    // Iterate to the last buf
    while (*next) { next = &(*next)->next; }

    // Set this one as the next
    *next = buffer;
  }
}

void _shift_buffer() {
    
  AudioBuffer *head = operating_buf;

  // If the head is already null
  if (!head) {
    // Just return
    TM_DEBUG("Head is already null somehow");
    return;
  }
  else {
    // Save the ref to the head
    AudioBuffer *old = head;

    // Set the new head to the next
    operating_buf = old->next;

    TM_DEBUG("Freeing the old...");
    // Clean any references to the previous transfer
    // If this is uncommented, the program crashes... but don't I need it?
    // free(old->tx_buf);
    free(old);

    // If there is another buffer to play
    if (operating_buf) {
      TM_DEBUG("New head is not null...");
      // Begin the next transfer
      _audio_watch_dreq();
    }

    return;
  }
}

// SPI.initialize MUST be initialized before calling this func
int audio_play_buffer(uint8_t chip_select, uint8_t dreq, const uint8_t *buf, uint32_t buf_len) {

  TM_DEBUG("called with args %d %d %d", chip_select, dreq, buf_len);
  // Create a new buffer struct
  AudioBuffer *new_buf = malloc(sizeof(AudioBuffer));
  // Set the next field
  new_buf->next = 0;
  // Malloc the space for the txbuf
  new_buf->tx_buf = malloc(buf_len);

  // If the malloc failed, return an error
  if (new_buf->tx_buf == NULL) {
    return -1;
  }

  // Copy over the bytes from the provided buffer
  memcpy(new_buf->tx_buf, buf, buf_len);
  // Set the length field
  new_buf->remaining_bytes = buf_len;

  // Set the chip select field
  new_buf->chip_select = chip_select;
  // Set the chip select pin as an output
  hw_digital_output(new_buf->chip_select);
  // Set the dreq field
  new_buf->dreq = dreq;
  // Set DREQ as an input
  hw_digital_input(new_buf->dreq);

  // If we have an existing operating buffer
  if (operating_buf) {
    TM_DEBUG("Using old interrupt id");
    // Use the same interrupt 
    new_buf->interrupt = operating_buf->interrupt;
  }
  // If this is the first audio buffer
  else {
    TM_DEBUG("Generating new interrupt id");
    // Check if there are any more interrupts available
    if (!hw_interrupts_available()) {
      // If not, return an error code
      return -2;
    }
    else {
      // If there are, grab the next available interrupt
      new_buf->interrupt = hw_interrupt_acquire();

      TM_DEBUG("Got a new one...%d", new_buf->interrupt);
    }
  }

  // Generate an ID for this stream so we know when it's complete
  new_buf->stream_id = _generate_stream_id();
  TM_DEBUG("generated new buf. Adding to queue");
  _queue_buffer(new_buf);

  return new_buf->stream_id;
}


// Called when a SPI transaction has completed
void _audio_spi_callback() {
  TM_DEBUG(" SPI Callback was hit!");
  // As long as we have an operating buf
  if (operating_buf != NULL) {
    // Check if we have more bytes to send for this buffer
    if (operating_buf->remaining_bytes > 0) {
      TM_DEBUG("We still have bytes remaining...");
      // Send more bytes when DREQ is low
      _audio_watch_dreq();
    }
    // We've completed playing all the bytes for this buffer
    else {

      // We need to wait for the transaction to totally finish
      // The DMA interrupt is fired too early by NXP so we need to
      // poll the BSY register until we can continue
      while (SSP_GetStatus(LPC_SSP0, SSP_STAT_BUSY)){};

      // Pull chip select back up
      hw_digital_write(operating_buf->chip_select, 1);
      TM_DEBUG("We're finished sending everything");
      uint16_t stream_id = operating_buf->stream_id;
      // Remove this buffer from the linked list and free the memory
      TM_DEBUG("Shifting...");
      _shift_buffer();
      TM_DEBUG("About to emit...");
      // Emit the event that we finished playing that buffer
      _audio_emit_completion(stream_id);
    }
  }
}

// Called when DREQ goes low. Data is available to be sent over SPI
void _audio_continue_spi() {
  TM_DEBUG("In the interrupt callback! %d", operating_buf->remaining_bytes);
  // If we have an operating buf
  if (operating_buf != NULL) {
    // Figure out how many bytes we're sending next
    uint8_t to_send = operating_buf->remaining_bytes < AUDIO_CHUNK_SIZE ? operating_buf->remaining_bytes : AUDIO_CHUNK_SIZE;
    TM_DEBUG("Going to send %d bytes...", operating_buf->remaining_bytes);
    // Pull chip select low
    hw_digital_write(operating_buf->chip_select, 0);
    // Transfer the data
    hw_spi_transfer(SPI_PORT, to_send, 0, operating_buf->tx_buf, NULL, -2, -2, &_audio_spi_callback);
    // Update our buffer position
    operating_buf->tx_buf += to_send;
    // Reduce the number of butes remaining
    operating_buf->remaining_bytes -= to_send;
  }
}

void _audio_watch_dreq() {
  // Start watching dreq for a low signal
  TM_DEBUG("Dreq read %d", hw_digital_read(operating_buf->dreq));
  if (!hw_digital_read(operating_buf->dreq)) {
    // Continue sending data
    _audio_continue_spi();
  }
  // If not
  else {
    // Wait for dreq to go low
    TM_DEBUG("just waiting for a low signal on %d for interrupt %d for %d.", operating_buf->dreq, operating_buf->interrupt, TM_INTERRUPT_MODE_LOW);
    hw_interrupt_watch(operating_buf->dreq, TM_INTERRUPT_MODE_LOW, operating_buf->interrupt, _audio_continue_spi);
  }
  
}

void _audio_emit_completion(uint16_t stream_id) {
  lua_State* L = tm_lua_state;
  if (!L) return;

  TM_DEBUG("Really emitting..");
  lua_getglobal(L, "_colony_emit");
  lua_pushstring(L, "audio_complete");
  lua_pushnumber(L, stream_id);
  tm_checked_call(L, 2);
  TM_DEBUG("Done being checked...");
}

uint16_t _generate_stream_id() {
  return ++master_id_gen;
}