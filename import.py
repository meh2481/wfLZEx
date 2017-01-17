# Convenience functions
import struct
import mathutils
import bpy

# read unsigned byte from file
def read_byte(file_object, endian = '<'):
    data = struct.unpack(endian+'B', file_object.read(1))[0]
    return data

# read unsgned short from file
def read_short(file_object, endian = '<'):
    data = struct.unpack(endian+'H', file_object.read(2))[0]
    return data

# read unsigned integer from file
def read_uint(file_object, endian = '<'):
    data = struct.unpack(endian+'I', file_object.read(4))[0]
    return data

# read signed integer from file
def read_int(file_object, endian = '<'):
    data = struct.unpack(endian+'i', file_object.read(4))[0]
    return data

# read floating point number from file
def read_float(file_object, endian = '<'):
    data = struct.unpack(endian+'f', file_object.read(4))[0]
    return data

print('Importing bones...')

# Create Armature and Object
bpy.ops.object.add(type='ARMATURE', enter_editmode=True)
object = bpy.context.object
object.name = 'armature'
armature = object.data
armature.name = 'armature'

#Read our input file
file = open('filename.bones', 'rb') # 'rb' - open for reading in binary mode
boneCount = read_uint(file)

for b in range(0, boneCount):
    matVals = []
    for x in range(0, 4):
        curMat = []
        for y in range(0, 4):
            val = read_float(file)
            curMat.append(val)
        matVals.append(curMat)
    
    matrix = mathutils.Matrix(matVals).inverted()
    bone = armature.edit_bones.new('bone' + str(b))
    bone.tail = mathutils.Vector([1,0,0])
    bone.transform(matrix)
        
# get out of edit mode when done
bpy.ops.object.mode_set(mode='OBJECT')

print('Done importing bones')