#!/usr/bin/env python3
"""
EMBODIOS Voice Simulation Demo
Shows how voice commands would work (text-based simulation)
"""

import time
import random
from typing import List, Tuple

class VoiceSimulator:
    """Simulates voice-to-text for demo purposes"""
    
    def __init__(self):
        self.ambient_noise = 0.1  # Simulate background noise
        self.recognition_accuracy = 0.95
        
    def simulate_voice_input(self, text: str) -> Tuple[str, float]:
        """Simulate voice recognition with timing and accuracy"""
        
        # Simulate speech duration (150 words per minute)
        words = text.split()
        speech_duration = len(words) * 0.4  # seconds per word
        
        print("🎤 [Listening...]")
        
        # Simulate speaking
        for i, word in enumerate(words):
            print(f"🔊 {word}", end=' ', flush=True)
            time.sleep(0.4)
            
        print("\n🎤 [Processing...]")
        time.sleep(0.5)  # Processing delay
        
        # Simulate recognition errors
        if random.random() > self.recognition_accuracy:
            # Introduce error
            error_word = random.choice(words)
            similar_words = {
                "GPIO": "GPI",
                "pin": "pen", 
                "on": "one",
                "off": "of",
                "LED": "lead",
                "seventeen": "seventy"
            }
            
            for orig, error in similar_words.items():
                if orig in text:
                    text = text.replace(orig, error, 1)
                    print(f"⚠️  Recognition error: '{orig}' → '{error}'")
                    break
                    
        confidence = random.uniform(0.85, 0.99)
        print(f"✅ Recognized (confidence: {confidence:.1%})")
        
        return text, speech_duration

class EMBODIOSVoiceDemo:
    """Demo showing voice control of EMBODIOS"""
    
    def __init__(self):
        self.voice_sim = VoiceSimulator()
        self.commands = [
            "Turn on GPIO pin seventeen",
            "Show system status",
            "Blink the LED three times",
            "Read temperature sensor",
            "Calculate forty two times pi",
            "List active devices"
        ]
        
    def run_demo(self):
        """Run interactive voice demo"""
        print("=" * 60)
        print("EMBODIOS Voice Control Demo (Simulation)")
        print("=" * 60)
        print("\nThis demonstrates how voice control would work in EMBODIOS.")
        print("Note: This is a text-based simulation. Real voice control")
        print("would require audio hardware drivers and speech recognition.\n")
        
        print("Available voice commands:")
        for i, cmd in enumerate(self.commands, 1):
            print(f"  {i}. \"{cmd}\"")
            
        print("\nPress Enter to simulate voice command, or 'q' to quit\n")
        
        while True:
            choice = input("Select command (1-6) or Enter for random: ").strip()
            
            if choice.lower() == 'q':
                break
                
            if choice.isdigit() and 1 <= int(choice) <= len(self.commands):
                command = self.commands[int(choice) - 1]
            else:
                command = random.choice(self.commands)
                
            print(f"\n--- Simulating voice command ---")
            print(f"You would say: \"{command}\"")
            print()
            
            # Simulate voice recognition
            recognized_text, duration = self.voice_sim.simulate_voice_input(command)
            
            # Show what EMBODIOS receives
            print(f"\n📝 EMBODIOS receives: \"{recognized_text}\"")
            
            # Simulate EMBODIOS processing
            self._process_command(recognized_text)
            
            print("\n" + "-" * 40 + "\n")
            
    def _process_command(self, text: str):
        """Simulate EMBODIOS processing the command"""
        print("\n🤖 EMBODIOS processing...")
        time.sleep(0.5)
        
        text_lower = text.lower()
        
        if "gpio" in text_lower or "led" in text_lower:
            if "on" in text_lower or "high" in text_lower:
                print("AI: Setting GPIO pin to HIGH")
                print("[HARDWARE] GPIO Pin 17 -> HIGH")
                print("💡 LED is now ON")
            elif "off" in text_lower or "low" in text_lower:
                print("AI: Setting GPIO pin to LOW")
                print("[HARDWARE] GPIO Pin 17 -> LOW")
                print("💡 LED is now OFF")
            elif "blink" in text_lower:
                print("AI: Blinking LED...")
                for i in range(3):
                    print(f"[HARDWARE] Blink {i+1}/3")
                    time.sleep(0.3)
                    
        elif "status" in text_lower:
            print("AI: System Status Report")
            print("[SYSTEM] CPU: 2 cores @ 2.4GHz")
            print("[SYSTEM] Memory: 1.2GB / 2.0GB")
            print("[SYSTEM] Uptime: 5 minutes")
            print("[SYSTEM] Model: TinyLlama-1.1B")
            
        elif "temperature" in text_lower:
            temp = random.uniform(20, 25)
            print("AI: Reading temperature sensor...")
            print(f"[SENSOR] Temperature: {temp:.1f}°C")
            
        elif "calculate" in text_lower:
            print("AI: Calculating 42 × π")
            result = 42 * 3.14159
            print(f"AI: The result is {result:.2f}")
            
        elif "device" in text_lower or "list" in text_lower:
            print("AI: Active devices:")
            print("[DEVICE] GPIO Controller - OK")
            print("[DEVICE] UART0 - 115200 baud")
            print("[DEVICE] I2C Bus - 400kHz")
            print("[DEVICE] Temperature Sensor - Connected")
        else:
            print("AI: Command processed")

def main():
    """Run voice simulation demo"""
    demo = EMBODIOSVoiceDemo()
    
    # Add speech synthesis simulation option
    import sys
    
    if len(sys.argv) > 1 and sys.argv[1] == "--tts":
        print("Text-to-Speech simulation enabled")
        print("(In real implementation, this would use audio output)")
        
    demo.run_demo()

if __name__ == "__main__":
    main()