#pragma once

#include <string>

namespace dxvk {
  
  /**
   * \brief DXVK error
   * 
   * A generic exception class that stores a
   * message. Exceptions should be logged.
   */
  class DxvkError {
    
  public:
    
    DxvkError() { }
    DxvkError(std::string&& message)
    : m_message(std::move(message)) { }
    
    const std::string& message() const {
      return m_message;
    }
    
  private:
    
    std::string m_message;
    
  };

  // Resource creation ran out of backing memory, rather than receiving
  // an invalid resource description. API frontends can report this separately.
  class DxvkOutOfMemory : public DxvkError {
  public:
    using DxvkError::DxvkError;
  };
  
}
