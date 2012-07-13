#include <stdio.h>
#include <getopt.h>
#include <expat.h>
#include <iostream>

#define EIGEN_NO_DEBUG
//#define EIGEN_DONT_VECTORIZE
//#define EIGEN_DONT_PARALLELIZE  // don't use openmp

#include <Eigen/Core>
#include <Eigen/LU>
#include <Eigen/SVD>

using namespace Eigen;

#define MAX_DEPTH	16

enum {
	ELEMENT_L1_CONFIGURATION = 0,
	ELEMENT_CRAFT,
	ELEMENT_PORTS,
	ELEMENT_PORT,
	ELEMENT_DISTANCE,
	ELEMENT_MASS,
	ELEMENT_MOTOR,
	ELEMENT_ARM,
	ELEMENT_ESC,
	ELEMENT_CUBE,
	ELEMENT_NUM
};

const char *elementNames[] = {
	"l1_configuration",
	"craft",
	"ports",
	"port",
	"distance",
	"mass",
	"motor",
	"arm",
	"esc",
	"cube"
};

enum {
	CONFIG_QUAD_PLUS = 0,
	CONFIG_QUAD_X,
	CONFIG_HEX_PLUS,
	CONFIG_NUM
};

const char *configTypes[] = {
	"quad_plus",
	"quad_x",
	"hex_plus"
};

typedef struct {
	int elementIds[MAX_DEPTH];
	int level;
	int validCraft;
	int n;
	char value[256];
	int valueLen;
} parseContext_t;

typedef struct {
	char craftId[256];
	int craftType;
	int n;				// number of motors
	double totalMass;
	Vector3d offsetCG;
	MatrixXd ports;
	MatrixXd propDir;
	MatrixXd motorX;
	MatrixXd motorY;
	double massEsc;
	VectorXd massEscs;
	double massMot;
	VectorXd massMots;
	double massArm;
	VectorXd massArms;
	VectorXd massObjects;
	double distMot;
	double distEsc;
	VectorXd angleArms;
	VectorXd distMots;
	VectorXd distEscs;
	MatrixXd objectsDim;
	MatrixXd objectsOffset;
	MatrixXd PITCH, ROLL, YAW, THROT;
	MatrixXd PD, M, Mt, PID;
	double jX, jY, jZ;
	Matrix3d J;
} l1Data_t;

l1Data_t l1Data;
int outputPID;
FILE *outFP;

template<typename _Matrix_Type_>
void displayMatrix(const char *name, _Matrix_Type_ &m) {
	int i, j;

	fprintf(outFP, "%s = [\n", name);
	for (i = 0; i < m.rows(); i++) {
		fprintf(outFP, "\t");
		for (j = 0; j < m.cols(); j++)
			fprintf(outFP, "%+9.4f  ", m(i, j));
		fprintf(outFP, "\n");
	}
	fprintf(outFP, "];\n");
}

template<typename _Matrix_Type_>
bool pseudoInverse(const _Matrix_Type_ &a, _Matrix_Type_ &result, double epsilon = std::numeric_limits<typename _Matrix_Type_::Scalar>::epsilon()) {
	Eigen::JacobiSVD<_Matrix_Type_> svd = a.jacobiSvd(Eigen::ComputeThinU |Eigen::ComputeThinV);

	typename _Matrix_Type_::Scalar tolerance =
		epsilon * std::max(a.cols(), a.rows()) * svd.singularValues().array().abs().maxCoeff();

	result = svd.matrixV() * _Matrix_Type_( (svd.singularValues().array().abs() > tolerance).select(svd.singularValues().
	array().inverse(), 0) ).asDiagonal() * svd.matrixU().adjoint();
}

int l1ToolFindPort(int port) {
	int i;

	for (i = 0; i < l1Data.ports.size(); i++)
		if (l1Data.ports(i) == port)
			return i;

	return -1;
}

int l1ToolConfigTypeByName(const XML_Char *name) {
	int i;

	for (i = 0; i < CONFIG_NUM; i++)
		if (!strncasecmp(name, configTypes[i], strlen(configTypes[i])+1))
			return i;

	return -1;
}

int l1ToolElementIdByName(const XML_Char *name) {
	int i;

	for (i = 0; i < ELEMENT_NUM; i++)
		if (!strncasecmp(name, elementNames[i], strlen(elementNames[i])+1))
			return i;

	return -1;
}

const XML_Char *l1ToolFindAttr(const XML_Char **atts, const char *name) {
	const XML_Char *value = NULL;

	while (*atts) {
		if (!strncasecmp(name, *atts, strlen(name))) {
			value = (const XML_Char *)*(++atts);
			break;
		}
		atts += 2;
	}

	return value;
}

void parse(XML_Parser parser, char c, int isFinal) {
	if (XML_STATUS_OK == XML_Parse(parser, &c, isFinal ^ 1, isFinal))
		return;

	fprintf(stderr, "l1Tool: parsing XML failed at line %lu, pos %lu: %s\n",
		(unsigned long)XML_GetCurrentLineNumber(parser),
		(unsigned long)XML_GetCurrentColumnNumber(parser),
		XML_ErrorString(XML_GetErrorCode(parser)) );

	exit(1);
}

void resetCraft(parseContext_t *context) {
	int n;

	switch (l1Data.craftType) {
		case CONFIG_QUAD_PLUS:
			n = 4;
			break;
		case CONFIG_QUAD_X:
			n = 4;
			break;
		case CONFIG_HEX_PLUS:
			n = 6;
			break;
		default:
			context->validCraft = 0;
			break;
	}
	l1Data.n = n;

	l1Data.ports.setZero(1, n);
	l1Data.propDir.setZero(1, n);
	l1Data.massMots.setZero(n);
	l1Data.massEscs.setZero(n);
	l1Data.massArms.setZero(n);
}

void parseCube(parseContext_t *context, const XML_Char **atts) {
	const XML_Char *att;
	
	l1Data.massObjects.conservativeResize(context->n+1);
	l1Data.objectsDim.conservativeResize(context->n+1, 3);
	l1Data.objectsOffset.conservativeResize(context->n+1, 3);

	att = l1ToolFindAttr(atts, "dimx");
	if (att)
		l1Data.objectsDim(context->n, 0) = atof(att);
	att = l1ToolFindAttr(atts, "dimy");
	if (att)
		l1Data.objectsDim(context->n, 1) = atof(att);
	att = l1ToolFindAttr(atts, "dimz");
	if (att)
		l1Data.objectsDim(context->n, 2) = atof(att);

	att = l1ToolFindAttr(atts, "offsetx");
	if (att)
		l1Data.objectsOffset(context->n, 0) = atof(att);
	att = l1ToolFindAttr(atts, "offsety");
	if (att)
		l1Data.objectsOffset(context->n, 1) = atof(att);
	att = l1ToolFindAttr(atts, "offsetz");
	if (att)
		l1Data.objectsOffset(context->n, 2) = atof(att);
}

void parsePort(parseContext_t *context, const XML_Char **atts) {
	const XML_Char *att;
	
	att = l1ToolFindAttr(atts, "rotation");
	if (!att) {
		fprintf(stderr, "l1Tool: craft '%s' missing config type\n", l1Data.craftId);
	}
	else {
		l1Data.propDir(0, context->n) = atoi(att);
	}
}

void parseCraft(parseContext_t *context, const XML_Char **atts) {
	const XML_Char *att;

	att = l1ToolFindAttr(atts, "id");
	if (att && (!*l1Data.craftId || !strcmp(att, l1Data.craftId))) {
		strncpy(l1Data.craftId, att, strlen(att));

		att = l1ToolFindAttr(atts, "config");
		if (!att) {
			fprintf(stderr, "l1Tool: craft '%s' missing config type\n", l1Data.craftId);
		}
		else {
			l1Data.craftType = l1ToolConfigTypeByName(att);
			if (l1Data.craftType < 0) {
				fprintf(stderr, "l1Tool: craft '%s' invalid config type '%s'\n", l1Data.craftId, att);
			}
			else {
				context->validCraft = 1;
				resetCraft(context);
			}
		}
	}
}

void XMLCALL startElement(void *ctx, const XML_Char *name, const XML_Char **atts ) {
	parseContext_t *context = (parseContext_t *)ctx;
	int elementId;

	elementId = l1ToolElementIdByName(name);

	if (elementId >= 0 && context->level < (MAX_DEPTH-1)) {
		context->elementIds[++context->level] = elementId;
//		printf("%d: %s\n", context->level, name);

		switch (context->elementIds[context->level]) {
			case ELEMENT_L1_CONFIGURATION:
				context->validCraft = 0;
				break;
			case ELEMENT_CRAFT:
				parseCraft(context, atts);
				break;
			case ELEMENT_PORTS:
				if (context->validCraft)
					context->n = 0;
				break;
			case ELEMENT_PORT:
				if (context->validCraft)
					parsePort(context, atts);
				break;
			case ELEMENT_DISTANCE:
				break;
			case ELEMENT_MASS:
				if (context->validCraft)
					context->n = 0;
				break;
			case ELEMENT_MOTOR:
				break;
			case ELEMENT_ARM:
				break;
			case ELEMENT_ESC:
				break;
			case ELEMENT_CUBE:
				if (context->validCraft)
					parseCube(context, atts);
				break;
		}
//		while (*atts)
//			printf("\tatt: %s\n", *atts++);
	}
	memset(context->value, 0, sizeof(context->value));
	context->valueLen = 0;
}

void XMLCALL parseChar(void *ctx, const XML_Char *str, int n) {
	parseContext_t *context = (parseContext_t *)ctx;

	context->value[context->valueLen++] = *str;
}

void XMLCALL endElement(void *ctx, const XML_Char *name __attribute__((__unused__)) ) {
	parseContext_t *context = (parseContext_t *)ctx;

	switch (context->elementIds[context->level]) {
		case ELEMENT_L1_CONFIGURATION:
			break;
		case ELEMENT_CRAFT:
			context->validCraft = 0;
			break;
		case ELEMENT_PORTS:
			break;
		case ELEMENT_PORT:
			if (context->validCraft) {
				l1Data.ports(0, context->n) = atoi(context->value);
				context->n++;
			}
			break;
		case ELEMENT_DISTANCE:
			break;
		case ELEMENT_MASS:
			break;
		case ELEMENT_MOTOR:
			if (context->validCraft) {
				if (context->elementIds[context->level-1] == ELEMENT_MASS)
					l1Data.massMot = atof(context->value);
				else if (context->elementIds[context->level-1] == ELEMENT_DISTANCE)
					l1Data.distMot = atof(context->value);
			}
			break;
		case ELEMENT_ARM:
			if (context->validCraft) {
				if (context->elementIds[context->level-1] == ELEMENT_MASS)
					l1Data.massArm = atof(context->value);
			}
			break;
		case ELEMENT_ESC:
			if (context->validCraft) {
				if (context->elementIds[context->level-1] == ELEMENT_MASS)
					l1Data.massEsc = atof(context->value);
				else if (context->elementIds[context->level-1] == ELEMENT_DISTANCE)
					l1Data.distEsc = atof(context->value);
			}
			break;
		case ELEMENT_CUBE:
			if (context->validCraft) {
				l1Data.massObjects(context->n) = atoi(context->value);
				context->n++;
			}
			break;
	}

	context->level--;
}

void l1ToolUsage(void) {
	fprintf(stderr, "usage: l1Tool [-h | --help] [-c | --craft-id <craft_id>] [-p | --pid] [-o | --ouput <output_file>] xml_file\n");
}

unsigned int l1ToolOptions(int argc, char **argv) {
        int ch;

        /* options descriptor */
        static struct option longopts[] = {
                { "help",       no_argument,            NULL,           'h' },
                { "craft-id",	required_argument,      NULL,           'c' },
                { "pid",	no_argument,		NULL,           'p' },
                { "output",	required_argument,      NULL,           'o' },
                { NULL,         0,                      NULL,           0 }
        };

	outFP = stdout;

        while ((ch = getopt_long(argc, argv, "hpo:c:", longopts, NULL)) != -1)
                switch (ch) {
                case 'h':
                        l1ToolUsage();
                        exit(0);
                        break;
		case 'p':
			outputPID = 1;
			break;
		case 'o':
			outFP = fopen(optarg, "w");
			if (outFP == NULL) {
				fprintf(stderr, "l1Tool: cannot open output file '%s'\n", optarg);
				exit(0);
			}
			break;
                case 'c':
                        strncpy(l1Data.craftId, optarg, sizeof(l1Data.craftId));
                        break;
                default:
                        l1ToolUsage();
                        return 0;
        }

        return 1;
}

int l1ToolReadXML(FILE *fp) {
	XML_Parser parser;
	parseContext_t context;
	char c;

	if (!(parser = XML_ParserCreate(NULL))) {
		fprintf(stderr, "l1Tool: cannot create XML parser, aborting\n");
		return -1;
	}

	memset(&context, 0, sizeof(parseContext_t));
	XML_SetUserData(parser, &context);

	XML_SetStartElementHandler(parser, &startElement);
	XML_SetDefaultHandler(parser, &parseChar);
	XML_SetEndElementHandler(parser, &endElement);

	while ((c = fgetc(fp)) != EOF)
		parse(parser, c, 0);

	parse(parser, c, 1);

	XML_ParserFree(parser);

	return 0;
}

typedef struct {
	double mass;
	double x, y, z;
	double dimX, dimY, dimZ;
} object_t;

void l1ToolJCalc(Matrix3d &J, double mass, double x, double y, double z) {
	Matrix3d S;

/* Original from Wiki that has the x & y reversed to our standard
	S <<	0,	-z,	y,
		z,	0,	-x,
		-y,	x,	0;
*/

	S <<	0,	-z,	x,
		z,	0,	-y,
		-x,	y,	0;

	J = J - mass*(S*S);
}

// only cuboid so far
void l1ToolShapeCalc(Matrix3d &J, object_t *obj) {
	double mass;
	int x, y, z;
	int i, j, k;
	
	// divide dimentions into mm cubes
	x = obj->dimX * 1000;
	y = obj->dimY * 1000;
	z = obj->dimZ * 1000;

	mass = obj->mass / (double)(x * y * z);
//printf("(%d, %d, %d) total mass %f point-mass used: %e\n", x, y, z, obj->mass, mass);

	for (i = 0; i < x; i++)
		for (j = 0; j < y; j++)
			for (k = 0; k < z; k++)
				l1ToolJCalc(J, mass,
					obj->x - l1Data.offsetCG(0) - obj->dimX/2.0 + (double)i/1000.0,
					obj->y - l1Data.offsetCG(1) - obj->dimX/2.0 + (double)j/1000.0,
					obj->z - l1Data.offsetCG(2) - obj->dimX/2.0 + (double)k/1000.0);
}

void l1ToolObjCalc(VectorXd frameX, VectorXd frameY) {
	object_t objs[256];
	int o;
	int i;

	memset(objs, 0, sizeof(objs));

	o = 0;
	for (i = 0; i < l1Data.n; i++) {
		// Motor
		objs[o].mass = l1Data.massMot;
		objs[o].x = frameX(i) * l1Data.distMot;
		objs[o].y = frameY(i) * l1Data.distMot;
		objs[o].z = 0.0;
		o++;

		// ESC
		objs[o].mass = l1Data.massEsc;
		objs[o].x = frameX(i) * l1Data.distEsc;
		objs[o].y = frameY(i) * l1Data.distEsc;
		objs[o].z = 0.0;
		o++;

		// ARM
		objs[o].mass = l1Data.massArm;
		objs[o].x = frameX(i) * l1Data.distMot / 2.0;
		objs[o].y = frameY(i) * l1Data.distMot / 2.0;
		objs[o].z = 0.0;
		o++;
	}

	for (i = 0; i < l1Data.massObjects.size(); i++) {
		objs[o].mass = l1Data.massObjects(i);
		objs[o].x = l1Data.objectsOffset(i, 0);
		objs[o].y = l1Data.objectsOffset(i, 1);
		objs[o].z = l1Data.objectsOffset(i, 2);

		objs[o].dimX = l1Data.objectsDim(i, 0);
		objs[o].dimY = l1Data.objectsDim(i, 1);
		objs[o].dimZ = l1Data.objectsDim(i, 2);
		o++;
	}

	l1Data.totalMass = 0.0;
	l1Data.offsetCG.setZero();
	for (i = 0; i < o; i++) {
		objs[i].mass /= 1000;			// g => Kg
		l1Data.offsetCG(0) += objs[i].mass * objs[i].x;
		l1Data.offsetCG(1) += objs[i].mass * objs[i].y;
		l1Data.offsetCG(2) += objs[i].mass * objs[i].z;

		l1Data.totalMass += objs[i].mass;
	}
	l1Data.offsetCG /= l1Data.totalMass;

	printf("MASS [%d objs] = %f Kg\n", o, l1Data.totalMass);
	printf("CG Offset = %f, %f, %f\n", l1Data.offsetCG(0), l1Data.offsetCG(1), l1Data.offsetCG(2));

	// calculate J matrix
	l1Data.J.setZero();
	for (i = 0; i < o; i++)
		if (objs[i].dimX != 0.0 && objs[i].dimY != 0.0 && objs[i].dimZ != 0.0)
			//  dimentioned shapes
			l1ToolShapeCalc(l1Data.J, &objs[i]);
		else
			// point masses
			l1ToolJCalc(l1Data.J, objs[i].mass, objs[i].x - l1Data.offsetCG(0), objs[i].y - l1Data.offsetCG(1), objs[i].z - l1Data.offsetCG(2));
}

void l1ToolCalc(void) {
	VectorXd frameX, frameY;
	MatrixXd A;
	MatrixXd B;
	float t, p, r, y;
	int i, j;

	l1Data.motorX.resize(1, l1Data.n);
	l1Data.motorY.resize(1, l1Data.n);
	l1Data.distMots.resize(l1Data.n);
	l1Data.distEscs.resize(l1Data.n);
	l1Data.angleArms.resize(l1Data.n);

	frameX.resize(l1Data.n);
	frameY.resize(l1Data.n);

	// calculate x/y coordinates for each motor
	switch (l1Data.craftType) {
		case CONFIG_QUAD_PLUS:
			frameX << 0.0, 1.0, 0.0, -1.0;
			frameY << 1.0, 0.0, -1.0, 0.0;
			break;
		case CONFIG_QUAD_X:
			frameX << -sqrt(2.0)/2.0, sqrt(2.0)/2.0, sqrt(2.0)/2.0, -sqrt(2.0)/2.0;
			frameY << sqrt(2.0)/2.0, sqrt(2.0)/2.0, -sqrt(2.0)/2.0, -sqrt(2.0)/2.0;
			break;
		case CONFIG_HEX_PLUS:
			frameX << 0.0,	sqrt(3.0)/2.0,	sqrt(3.0)/2.0,	0.0,	-sqrt(3.0)/2.0,	-sqrt(3.0)/2.0;
			frameY << 1.0,	0.5,		-0.5,		-1.0,	-0.5,		0.5;
			break;
	}

	// calc GG offset & J matrix
	l1ToolObjCalc(frameX, frameY);

	l1Data.motorX = (frameX.transpose() * l1Data.distMot) * -1.0;
	l1Data.motorY = (frameY.transpose() * l1Data.distMot);

	// adjust for CG offset
	l1Data.motorX -= VectorXd::Ones(l1Data.n) * l1Data.offsetCG(0);
	l1Data.motorY -= VectorXd::Ones(l1Data.n) * l1Data.offsetCG(1);

	l1Data.propDir *= -1.0;								// our sense of rotation is counter intuative

	for (i = 0; i < l1Data.n; i++) {
		l1Data.distMots(i) = sqrt(l1Data.motorX(i)*l1Data.motorX(i) + l1Data.motorY(i)*l1Data.motorY(i));
		l1Data.angleArms(i) = fabs(atan(l1Data.motorX(i) / l1Data.motorY(i)));
		l1Data.distEscs(i) = l1Data.distMots(i) * (l1Data.distEsc / l1Data.distMot);
	}

	A.resize(3, l1Data.n);
	B.resize(3, 1);

	// Roll
	A <<	l1Data.motorY,
		MatrixXd::Ones(1, l1Data.n),
		l1Data.motorX;
	B <<	0,
		0,
		1;

	pseudoInverse(A, l1Data.ROLL);
	l1Data.ROLL *= B;

	// Pitch
	A <<	l1Data.motorX,
		MatrixXd::Ones(1, l1Data.n),
		l1Data.motorY;
	B <<	0,
		0,
		1;

	pseudoInverse(A, l1Data.PITCH);
	l1Data.PITCH *= B;

	// Yaw
	A <<	l1Data.motorX,
		l1Data.motorY,
		l1Data.propDir;
	B <<	0,
		0,
		1;

	pseudoInverse(A, l1Data.YAW);
	l1Data.YAW *= B;

	// Throttle
	A.resize(4, l1Data.n);
	B.resize(4, 1);

	A <<	l1Data.motorX,
		l1Data.motorY,
		l1Data.propDir,
		MatrixXd::Ones(1, l1Data.n);
	B <<	0,
		0,
		0,
		l1Data.n;

	pseudoInverse(A, l1Data.THROT);
	l1Data.THROT *= B;


	// PD
	l1Data.PD.resize(l1Data.n, 3);
	l1Data.PD <<	l1Data.ROLL,
			l1Data.PITCH,
			l1Data.YAW,

	// M
	l1Data.M.resize(3, l1Data.n);
	l1Data.M <<	l1Data.motorX,
			l1Data.motorY,
			l1Data.propDir;

	// Mt
	l1Data.Mt.resize(l1Data.n, 4);
	l1Data.Mt << l1Data.THROT, l1Data.PD * (l1Data.M*l1Data.PD).inverse();

	// PID
	l1Data.PID.setZero(4, l1Data.n);
	l1Data.PID <<	l1Data.Mt.col(0).transpose() / l1Data.Mt.col(0).maxCoeff(),
			l1Data.Mt.col(2).transpose() / l1Data.Mt.col(2).maxCoeff(),
			l1Data.Mt.col(1).transpose() / l1Data.Mt.col(1).maxCoeff(),
			l1Data.Mt.col(3).transpose() * l1Data.n;
	l1Data.PID = l1Data.PID.transpose().eval() * 100.0;

	if (outputPID) {
		displayMatrix("PID", l1Data.PID);
		for (i = 1; i <= 14; i++) {
			t = 0.0;
			p = 0.0;
			r = 0.0;
			y = 0.0;

			j = l1ToolFindPort(i);
			if (j >= 0) {
				t = l1Data.PID(j, 0);
				p = l1Data.PID(j, 1);
				r = l1Data.PID(j, 2);
				y = l1Data.PID(j, 3);
			}
			fprintf(outFP, "#define DEFAULT_MOT_PWRD_%02d_T\t%+f\n", i, t);
			fprintf(outFP, "#define DEFAULT_MOT_PWRD_%02d_P\t%+f\n", i, p);
			fprintf(outFP, "#define DEFAULT_MOT_PWRD_%02d_R\t%+f\n", i, r);
			fprintf(outFP, "#define DEFAULT_MOT_PWRD_%02d_Y\t%+f\n", i, y);
		}
	}
	// otherwise show L1 results
	else {
		displayMatrix("Mt", l1Data.Mt);
		for (i = 1; i <= 14; i++) {
			t = 0.0;
			p = 0.0;
			r = 0.0;
			y = 0.0;

			j = l1ToolFindPort(i);
			if (j >= 0) {
				t = l1Data.Mt(j, 0);
				r = l1Data.Mt(j, 1);
				p = l1Data.Mt(j, 2);
				y = l1Data.Mt(j, 3);
			}
			fprintf(outFP, "#define DEFAULT_MOT_PWRD_%02d_T\t%+f\n", i, t);
			fprintf(outFP, "#define DEFAULT_MOT_PWRD_%02d_P\t%+f\n", i, p);
			fprintf(outFP, "#define DEFAULT_MOT_PWRD_%02d_R\t%+f\n", i, r);
			fprintf(outFP, "#define DEFAULT_MOT_PWRD_%02d_Y\t%+f\n", i, y);
		}

		displayMatrix("M", l1Data.M);
		for (i = 1; i <= 14; i++) {
			r = 0.0;
			p = 0.0;
			y = 0.0;

			j = l1ToolFindPort(i);
			if (j >= 0) {
				r = l1Data.M(0, j);
				p = l1Data.M(1, j);
				y = l1Data.M(2, j);
			}
			fprintf(outFP, "#define DEFAULT_L1_ATT_MM_R%02d\t%+f\n", i, r);
			fprintf(outFP, "#define DEFAULT_L1_ATT_MM_P%02d\t%+f\n", i, p);
			fprintf(outFP, "#define DEFAULT_L1_ATT_MM_Y%02d\t%+f\n", i, y);
		}

		displayMatrix("J", l1Data.J);
		fprintf(outFP, "#define DEFAULT_L1_ATT_J_ROLL\t%f\n", l1Data.J(0, 0));
		fprintf(outFP, "#define DEFAULT_L1_ATT_J_PITCH\t%f\n", l1Data.J(1, 1));
		fprintf(outFP, "#define DEFAULT_L1_ATT_J_YAW\t%f\n", l1Data.J(2, 2));
	}
}

int main(int argc, char **argv) {
	FILE *fp;

        if (!l1ToolOptions(argc, argv)) {
                fprintf(stderr, "Init failed, aborting\n");
                return 0;
        }
        argc -= optind;
        argv += optind;

	if (argc < 1) {
		fprintf(stderr, "l1Tool: requires xml file argument, aborting\n");
		return -1;
	}
	if (!(fp = fopen(argv[0], "r"))) {
		fprintf(stderr, "l1Tool: cannot open XML file '%s', aborting\n", argv[1]);
		return -1;
	}

	if (l1ToolReadXML(fp) < 0)
		return -1;

	fprintf(outFP, "Craft: '%s'\n", l1Data.craftId);
	fprintf(outFP, "Motors: %d\n", l1Data.n);
/*
	std::cout << "l1Data.ports: " << l1Data.ports << std::endl;
	std::cout << "l1Data.propDir: " << l1Data.propDir << std::endl;
	std::cout << "l1Data.distMot: " << l1Data.distMot << std::endl;
	std::cout << "l1Data.distEsc: " << l1Data.distEsc << std::endl;
	std::cout << "l1Data.massMot: " << l1Data.massMot << std::endl;
	std::cout << "l1Data.massEsc: " << l1Data.massEsc << std::endl;
	std::cout << "l1Data.massArm: " << l1Data.massArm << std::endl;
	std::cout << "l1Data.massObjects: " << l1Data.massObjects << std::endl;
	std::cout << "l1Data.objectsDim: " << l1Data.objectsDim << std::endl;
	std::cout << "l1Data.objectsOffset: " << l1Data.objectsOffset << std::endl;
*/

	l1ToolCalc();

	return 0;
}
