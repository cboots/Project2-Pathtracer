// CIS565 CUDA Raytracer: A parallel raytracer for Patrick Cozzi's CIS565: GPU Computing at the University of Pennsylvania
// Written by Yining Karl Li, Copyright (c) 2012 University of Pennsylvania
// This file includes code from:
//       Rob Farber for CUDA-GL interop, from CUDA Supercomputing For The Masses: http://www.drdobbs.com/architecture-and-design/cuda-supercomputing-for-the-masses-part/222600097
//       Varun Sampath and Patrick Cozzi for GLSL Loading, from CIS565 Spring 2012 HW5 at the University of Pennsylvania: http://cis565-spring-2012.github.com/
//       Yining Karl Li's TAKUA Render, a massively parallel pathtracing renderer: http://www.yiningkarlli.com

#include "main.h"

//-------------------------------
//-------------MAIN--------------
//-------------------------------

int main(int argc, char** argv){

#ifdef __APPLE__
	// Needed in OSX to force use of OpenGL3.2 
	glfwOpenWindowHint(GLFW_OPENGL_VERSION_MAJOR, 3);
	glfwOpenWindowHint(GLFW_OPENGL_VERSION_MINOR, 2);
	glfwOpenWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
	glfwOpenWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif



	// Set up pathtracer stuff
	bool loadedScene = false;
	finishedRender = false;

	targetFrame = 0;
	singleFrameMode = false;

	// Load scene file
	for(int i=1; i<argc; i++){
		string header; string data;
		istringstream liness(argv[i]);
		getline(liness, header, '='); getline(liness, data, '=');
		if(strcmp(header.c_str(), "scene")==0){
			renderScene = new scene(data);
			loadedScene = true;
		}else if(strcmp(header.c_str(), "frame")==0){
			targetFrame = atoi(data.c_str());
			singleFrameMode = true;
		}
	}

	if(!loadedScene){
		cout << "Error: scene file needed!" << endl;
		return 0;
	}

	//Print CUDA device info
	// Number of CUDA devices
	int devCount;
	cudaGetDeviceCount(&devCount);
	printf("CUDA Device Query...\n");
	printf("There are %d CUDA devices.\n", devCount);

	// Iterate through devices
	for (int i = 0; i < devCount; ++i)
	{
		// Get device properties
		printf("\nCUDA Device #%d\n", i);
		cudaDeviceProp devProp;
		cudaGetDeviceProperties(&devProp, i);
		utilityCore::printDevProp(devProp);
	}

	// Set up camera stuff from loaded pathtracer settings
	iterations = 0;
	frameFilterCounter = 0;
	renderCam = &renderScene->renderCam;
	width = renderCam->resolution[0];
	height = renderCam->resolution[1];


	//TODO: Set up rendering options
	renderOpts = new renderOptions();
	renderOpts->mode = TRACEDEPTH_DEBUG;
	renderOpts->traceDepth = 50;
	renderOpts->rayPoolSize =1.0f;//Size of pool relative to number of pixels. 1.0f means 1 ray per pixel
	renderOpts->stocasticRayAssignment = false;
	
	//Defaults
	renderOpts->globalLightGeomInd = -1;
	//Setup global lighting conditions
	for(int m = 0; m < renderScene->materials.size(); m++)
	{
		if(renderScene->materials[m].specularExponent < 0)//Flag for global light source
		{
			//If we have a global light material, look for a corresponding geom
			for(int g = 0; g <  renderScene->objects.size(); g++)
			{
				if(renderScene->objects[g].materialid == m)
				{
					renderOpts->globalLightGeomInd = g;
					break;
				}
			}
			if(renderOpts->globalLightGeomInd > -1)
				break;
		}
	}
	

	//Rendering toggle options
	renderOpts->antialiasing = true;
	renderOpts->streamCompaction = true;
	renderOpts->frameFiltering = true;
	renderOpts->globalShadows = true;


	renderOpts->airIOR = 1.0;
	renderOpts->airAbsorbtion = glm::vec3(0.0, 0.0, 0.0);//No air absorbtion effects for now
	renderOpts->minT = 0.001;

	if(targetFrame>=renderCam->frames){
		cout << "Warning: Specified target frame is out of range, defaulting to frame 0." << endl;
		targetFrame = 0;
	}

	// Launch CUDA/GL

#ifdef __APPLE__
	init();
#else
	init(argc, argv);
#endif

	initCuda();

	initVAO();
	initTextures();

	GLuint passthroughProgram;
	passthroughProgram = initShader("shaders/passthroughVS.glsl", "shaders/passthroughFS.glsl");

	glUseProgram(passthroughProgram);
	glActiveTexture(GL_TEXTURE0);

#ifdef __APPLE__
	// send into GLFW main loop
	while(1){
		display();
		if (glfwGetKey(GLFW_KEY_ESC) == GLFW_PRESS || !glfwGetWindowParam( GLFW_OPENED )){
			exit(0);
		}
	}

	glfwTerminate();
#else
	glutDisplayFunc(display);
	glutKeyboardFunc(keyboard);

	glutMainLoop();
#endif
	return 0;
}

//-------------------------------
//---------RUNTIME STUFF---------
//-------------------------------

void runCuda(){
	// Map OpenGL buffer object for writing from CUDA on a single GPU
	// No data is moved (Win & Linux). When mapped to CUDA, OpenGL should not use this buffer

	if(iterations<renderCam->iterations){
		uchar4 *dptr=NULL;
		iterations++;
		frameFilterCounter++;
		cudaGLMapBufferObject((void**)&dptr, pbo);

		//pack geom and material arrays
		geom* geoms = new geom[renderScene->objects.size()];
		material* materials = new material[renderScene->materials.size()];

		for(int i=0; i<renderScene->objects.size(); i++){
			geoms[i] = renderScene->objects[i];
		}
		for(int i=0; i<renderScene->materials.size(); i++){
			materials[i] = renderScene->materials[i];
		}

		//Measure frame rate
		static clock_t tic;
		clock_t toc = tic;
		tic = clock();
		//Slight Low pass filter to make FPS easier to read
		fps = 0.2*fps + 0.8*CLOCKS_PER_SEC/float(tic-toc);

		// execute the kernel

		cudaRaytraceCore(dptr, renderCam, renderOpts, targetFrame, iterations, frameFilterCounter, materials, renderScene->materials.size(), geoms, renderScene->objects.size() );

		// unmap buffer object
		cudaGLUnmapBufferObject(pbo);
	}else{

		if(!finishedRender){
			//output image file
			image outputImage(renderCam->resolution.x, renderCam->resolution.y);

			for(int x=0; x<renderCam->resolution.x; x++){
				for(int y=0; y<renderCam->resolution.y; y++){
					int index = x + (y * renderCam->resolution.x);
					outputImage.writePixelRGB(renderCam->resolution.x-1-x,y,renderCam->image[index]/float(frameFilterCounter));
				}
			}

			gammaSettings gamma;
			gamma.applyGamma = true;
			gamma.gamma = 1.25/1.5;
			gamma.divisor = 1.0;
			outputImage.setGammaSettings(gamma);
			string filename = renderCam->imageName;
			string s;
			stringstream out;
			out << targetFrame;
			s = out.str();
			utilityCore::replaceString(filename, ".bmp", "."+s+".bmp");
			utilityCore::replaceString(filename, ".png", "."+s+".png");
			outputImage.saveImageRGB(filename);
			cout << "Saved frame " << s << " to " << filename << endl;
			finishedRender = true;
			if(singleFrameMode==true){
				cudaDeviceReset(); 
				exit(0);
			}
		}
		if(targetFrame<renderCam->frames-1){

			//clear image buffer and move onto next frame
			targetFrame++;
			iterations = 0;
			for(int i=0; i<renderCam->resolution.x*renderCam->resolution.y; i++){
				renderCam->image[i] = glm::vec3(0,0,0);
			}
			cudaDeviceReset(); 
			finishedRender = false;
		}
	}


}

#ifdef __APPLE__

void display(){
	runCuda();

	string title = "CIS565 Render | " + utilityCore::convertIntToString(iterations) + " Iterations";
	glfwSetWindowTitle(title.c_str());

	glBindBuffer( GL_PIXEL_UNPACK_BUFFER, pbo);
	glBindTexture(GL_TEXTURE_2D, displayImage);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, 
		GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	glClear(GL_COLOR_BUFFER_BIT);   

	// VAO, shader program, and texture already bound
	glDrawElements(GL_TRIANGLES, 6,  GL_UNSIGNED_SHORT, 0);

	glfwSwapBuffers();
}

#else

void display(){


	runCuda();


	string title = "565Pathtracer | " + utilityCore::convertIntToString(iterations) + " Iterations | FPS " + utilityCore::convertIntToString(fps);
	glutSetWindowTitle(title.c_str());

	glBindBuffer( GL_PIXEL_UNPACK_BUFFER, pbo);
	glBindTexture(GL_TEXTURE_2D, displayImage);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, 
		GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	glClear(GL_COLOR_BUFFER_BIT);   

	// VAO, shader program, and texture already bound
	glDrawElements(GL_TRIANGLES, 6,  GL_UNSIGNED_SHORT, 0);

	glutPostRedisplay();
	glutSwapBuffers();
}

void keyboard(unsigned char key, int x, int y)
{
	std::cout << key << std::endl;
	switch (key) 
	{
	case(27):
		//Reset device to flush profiling data
		cudaDeviceReset(); 
		exit(0);
		break;
		///Mode selection options
	case '1':
		//Enter normal raytracing mode
		renderOpts->mode = PATHTRACE;
		cout << "Pathtracing Mode" <<endl;
		break;
	case '2':
		//Enter distance debug mode
		renderOpts->mode = RAYCOUNT_DEBUG;
		cout << "Ray Count Debug Mode" <<endl;
		break;
	case '3':
		//Enter trace depth debug mode
		renderOpts->mode = TRACEDEPTH_DEBUG;
		cout << "Trace Depth Debug Mode" <<endl;
		break;
	case '4':
		//Show first hit color
		renderOpts->mode = FIRST_HIT_DEBUG;
		cout << "First Hit Debug Mode" <<endl;
		break;
	case '5':
		//Enter normal debug mode
		renderOpts->mode = NORMAL_DEBUG;
		cout <<  "Normal Debug Mode" <<endl;
		break;
	case 'A':
		renderOpts->antialiasing = !renderOpts->antialiasing;
		cout << "Antialiasing: " << renderOpts->antialiasing<< endl;
		break;
	case 'S':
		renderOpts->streamCompaction = !renderOpts->streamCompaction;
		cout << "Stream Compaction: " << renderOpts->streamCompaction<< endl;
		break;
	case 'F':
		renderOpts->frameFiltering = !renderOpts->frameFiltering;
		frameFilterCounter = 0;
		cout << "Frame Filter: " << renderOpts->frameFiltering<< endl;
		break;
	case 'G':
		renderOpts->globalShadows = !renderOpts->globalShadows;
		cout << "Global Shadows: " << renderOpts->globalShadows<< endl;
		break;
	case 'f':
		frameFilterCounter = 0;
		cout << "Frame Filter Reset" << endl;
		break;
	case '=':
		renderOpts->traceDepth++;
		cout << "Trace Depth: " << renderOpts->traceDepth << endl;
		break;
	case '-':
		if(renderOpts->traceDepth > 1)
			renderOpts->traceDepth--;
		cout << "Trace Depth: " << renderOpts->traceDepth << endl;
		break;
	}
	//TODO: Add more keyboard controls here
}

#endif




//-------------------------------
//----------SETUP STUFF----------
//-------------------------------

#ifdef __APPLE__
void init(){

	if (glfwInit() != GL_TRUE){
		shut_down(1);      
	}

	// 16 bit color, no depth, alpha or stencil buffers, windowed
	if (glfwOpenWindow(width, height, 5, 6, 5, 0, 0, 0, GLFW_WINDOW) != GL_TRUE){
		shut_down(1);
	}

	// Set up vertex array object, texture stuff
	initVAO();
	initTextures();
}
#else
void init(int argc, char* argv[]){
	glutInit(&argc, argv);
	glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA);
	glutInitWindowSize(width, height);
	glutCreateWindow("565Raytracer");

	// Init GLEW
	glewInit();
	GLenum err = glewInit();
	if (GLEW_OK != err)
	{
		/* Problem: glewInit failed, something is seriously wrong. */
		std::cout << "glewInit failed, aborting." << std::endl;
		exit (1);
	}

	initVAO();
	initTextures();
}
#endif

void initPBO(GLuint* pbo){
	if (pbo) {
		// set up vertex data parameter
		int num_texels = width*height;
		int num_values = num_texels * 4;
		int size_tex_data = sizeof(GLubyte) * num_values;

		// Generate a buffer ID called a PBO (Pixel Buffer Object)
		glGenBuffers(1,pbo);
		// Make this the current UNPACK buffer (OpenGL is state-based)
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, *pbo);
		// Allocate data for the buffer. 4-channel 8-bit image
		glBufferData(GL_PIXEL_UNPACK_BUFFER, size_tex_data, NULL, GL_DYNAMIC_COPY);
		cudaGLRegisterBufferObject( *pbo );
	}
}

void initCuda(){
	// Use device with highest Gflops/s
	cudaGLSetGLDevice( compat_getMaxGflopsDeviceId() );

	initPBO(&pbo);

	// Clean up on program exit
	atexit(cleanupCuda);


	runCuda();
}

void initTextures(){
	glGenTextures(1,&displayImage);
	glBindTexture(GL_TEXTURE_2D, displayImage);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA,
		GL_UNSIGNED_BYTE, NULL);
}

void initVAO(void){
	GLfloat vertices[] =
	{ 
		-1.0f, -1.0f, 
		1.0f, -1.0f, 
		1.0f,  1.0f, 
		-1.0f,  1.0f, 
	};

	GLfloat texcoords[] = 
	{ 
		1.0f, 1.0f,
		0.0f, 1.0f,
		0.0f, 0.0f,
		1.0f, 0.0f
	};

	GLushort indices[] = { 0, 1, 3, 3, 1, 2 };

	GLuint vertexBufferObjID[3];
	glGenBuffers(3, vertexBufferObjID);

	glBindBuffer(GL_ARRAY_BUFFER, vertexBufferObjID[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	glVertexAttribPointer((GLuint)positionLocation, 2, GL_FLOAT, GL_FALSE, 0, 0); 
	glEnableVertexAttribArray(positionLocation);

	glBindBuffer(GL_ARRAY_BUFFER, vertexBufferObjID[1]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(texcoords), texcoords, GL_STATIC_DRAW);
	glVertexAttribPointer((GLuint)texcoordsLocation, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(texcoordsLocation);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vertexBufferObjID[2]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
}

GLuint initShader(const char *vertexShaderPath, const char *fragmentShaderPath){
	GLuint program = glslUtility::createProgram(vertexShaderPath, fragmentShaderPath, attributeLocations, 2);
	GLint location;

	glUseProgram(program);

	if ((location = glGetUniformLocation(program, "u_image")) != -1)
	{
		glUniform1i(location, 0);
	}

	return program;
}

//-------------------------------
//---------CLEANUP STUFF---------
//-------------------------------

void cleanupCuda(){
	if(pbo) deletePBO(&pbo);
	if(displayImage) deleteTexture(&displayImage);
}

void deletePBO(GLuint* pbo){
	if (pbo) {
		// unregister this buffer object with CUDA
		cudaGLUnregisterBufferObject(*pbo);

		glBindBuffer(GL_ARRAY_BUFFER, *pbo);
		glDeleteBuffers(1, pbo);

		*pbo = (GLuint)NULL;
	}
}

void deleteTexture(GLuint* tex){
	glDeleteTextures(1, tex);
	*tex = (GLuint)NULL;
}

void shut_down(int return_code){
#ifdef __APPLE__
	glfwTerminate();
#endif
	exit(return_code);
}
