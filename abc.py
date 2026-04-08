import base64

print("=== Base64 Encoder / Decoder ===")

choice = input("Choose option:\n1. Encode\n2. Decode\nEnter (1/2): ")

if choice == "1":
    text = input("Enter text to encode: ")
    
    encoded = base64.b64encode(text.encode()).decode()
    print("\nEncoded Output:", encoded)

elif choice == "2":
    encoded_text = input("Enter Base64 text to decode: ")
    
    try:
        decoded = base64.b64decode(encoded_text.encode()).decode()
        print("\nDecoded Output:", decoded)
    except Exception as e:
        print("\nInvalid Base64 input!")

else:
    print("\nInvalid choice! Please run again.")
